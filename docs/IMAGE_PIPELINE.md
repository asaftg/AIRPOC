# Image Pipeline (ISP + AE)

How a raw Y10 frame becomes a clean, well-exposed image, and the C++ production
plan for the hot path.

## Raw characteristics (measured)

- **Left-justified Y10**: 10-bit value = `pixel >> 6`.
- **Black level ≈ 60** (BLKLEVEL `0x3254` = `0x3c`). Subtract before scaling.
- **Row FPN is negligible**: measured raw per-row offset std ≈ **0.5 LSB** in a
  flat field (once the 1440 sensor ROI made the line 64-byte aligned — see
  `DRIVER.md`). No row correction is needed, and a content-derived one is harmful
  (see *"No row de-band"* below).
- At gain 0 a normally-lit room sits at mean ~80–180/1023; AE uses gain to reach
  the target (gain is 0.1 dB/step ⇒ exponential).

## Pipeline (current, in `imx296_preview.py`)

```
Y10 (>>6) → 3×3 median → black-level + adaptive-white tone map → γ=0.85 → 8-bit
```

### Auto-exposure (AE)
Meters mean of a subsampled luma; drives **exposure first** (low noise), raising
**gain** only when exposure is maxed; lowers gain first when too bright. Target
mean ≈ 450/1023. Controls are the driver's real v4l2 `exposure`(µs)/`gain`.

### No row de-band (removed 2026-06-30)
An earlier build ran a content-derived row correction (per-row median minus its
vertical low-pass, subtracted). **It was removed — it was the cause of the
"horizontal streaks in the sky" artifact, not a fix.** Why it fails:

```python
# WRONG — do not reintroduce:
rowmed = np.median(frame[:, ::4], axis=1)
rowsm  = np.convolve(rowmed, np.ones(31)/31, 'same')
frame -= (rowmed - rowsm)[:, None]
```
It cannot distinguish real horizontal scene edges from row noise. At a hard edge
(tree line vs bright sky) `rowmed` steps; the 31-row smoothing spreads that step
±15 rows, so it subtracts a large spurious offset (**measured std 13 LSB, up to
150 LSB**) across those rows → horizontal streaks that **track the scene as the
camera pans**. Meanwhile the real row FPN it was meant to fix is only ~0.5 LSB.
Verdict: negative SNR trade, remove entirely. If a future sensor ever shows real
row FPN, correct it with a **static dark-frame per-row offset** (calibrated once,
scene-independent), never a per-frame content-derived subtraction.

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
3. **Black-level** subtract (constant). No row de-band (raw FPN ~0.5 LSB; see
   above). If ever needed, a static calibrated per-row offset LUT — not content-derived.
4. **AE**: histogram/mean reduction → PID on exposure(µs)+gain via `VIDIOC_S_EXT_CTRLS`.
5. **Tone/gamma**: LUT (texture) → 8-bit (or keep 10-bit for detection).
6. **Output**: zero-copy NVMM buffer → encoder (Xavier AGX NVENC) / detector.

Mono ⇒ **no demosaic, no AWB** — strictly simpler than the seeker EO (IMX568)
color path. Reuse the EO AE/tone tuning as reference (read-only).

Budget at 1440×1088: the whole chain is a few hundred µs on the GPU, leaving the
16.6 ms/frame for detection + encode.
