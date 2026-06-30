# Image Pipeline (ISP + AE)

How a raw Y10 frame becomes a clean, well-exposed image, and the C++ production
plan for the hot path.

## Raw characteristics (measured)

- **Left-justified Y10**: 10-bit value = `pixel >> 6`.
- **Black level ≈ 60** (BLKLEVEL `0x3254` = `0x3c`). Subtract before scaling.
- **Horizontal row-noise / FPN**: a low-amplitude per-row offset that reads as
  wavy horizontal banding once tone-mapped. Must be corrected (see de-band).
- At gain 0 a normally-lit room sits at mean ~80–180/1023; AE uses gain to reach
  the target (gain is 0.1 dB/step ⇒ exponential).

## Pipeline (current, in `imx296_preview.py`)

```
Y10 (>>6) → 3×3 median → row-noise de-band → black-level + adaptive-white
            tone map → γ=0.85 → 8-bit
```

### Auto-exposure (AE)
Meters mean of a subsampled luma; drives **exposure first** (low noise), raising
**gain** only when exposure is maxed; lowers gain first when too bright. Target
mean ≈ 450/1023. Controls are the driver's real v4l2 `exposure`(µs)/`gain`.

### Row-noise de-band (the key quality step)
Per-row offset = high-frequency part of the per-row median:
```python
rowmed = np.median(frame[:, ::4], axis=1)          # robust per-row level
rowsm  = np.convolve(rowmed, np.ones(31)/31, 'same')  # vertical low-pass
frame -= (rowmed - rowsm)[:, None]                  # subtract only the banding
```
Removes the horizontal banding without touching real structure (verified
before/after). This is the single biggest "looks like a real camera" step.

### Tone map
`y8 = clip((frame − 60) × 255/(P99.5 − 60), 0, 255)` then γ LUT. Black anchored
at the sensor black level; white at the 99.5th percentile ⇒ punch without the
wash-out that a full percentile stretch causes on dim frames.

## Capture note (don't reintroduce)
The Python capture drains `v4l2-ctl --stream-to=-` in a **dedicated light reader
thread** (raw bytes only). The ISP runs in a separate thread. If the reader does
heavy work it gets GIL-starved, the pipe backpressures, and `v4l2-ctl`
overwrites buffers → torn frames. Keep the reader minimal.

## Production hot path (C++/CUDA — the "best quality, 60 fps" target)

Python tops out ~30 fps for the full ISP. Per `ENGINEERING_GUIDELINES.md`, the
shipping pipeline is C++ on the GPU:

1. **Capture**: V4L2 mmap dequeue (single fd; set exposure/gain on the same fd —
   eliminates the multi-open glitch the tools work around).
2. **Unpack** Y10→16-bit (`>>6`) — CUDA kernel or VPI.
3. **Black-level + de-band**: per-row reduction (median/mean) + subtract — a
   reduction kernel; trivial on GPU at 60 fps.
4. **AE**: histogram/mean reduction → PID on exposure(µs)+gain via `VIDIOC_S_EXT_CTRLS`.
5. **Tone/gamma**: LUT (texture) → 8-bit (or keep 10-bit for detection).
6. **Output**: zero-copy NVMM buffer → encoder (Xavier AGX NVENC) / detector.

Mono ⇒ **no demosaic, no AWB** — strictly simpler than the seeker EO (IMX568)
color path. Reuse the EO AE/tone tuning as reference (read-only).

Budget at 1456×1088: the whole chain is a few hundred µs on the GPU, leaving the
16.6 ms/frame for detection + encode.
