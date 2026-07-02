# EO Image Pipeline (ISP + AE)

How a raw Y10 frame becomes a clean, well-exposed image, and the production hot-path
plan. The live implementation is the preview tool `eo/tools/imx296_preview.py`
(Python bench tool); the shipping pipeline is C++/CUDA (below).

## Pipeline

```
Y10 (>>6) → [digital-zoom crop] → resize-to-display (INTER_AREA)
          → black-level subtract → adaptive-white tone map → γ=0.85 → 8-bit → MJPEG
```

Resizing before the tone map runs the per-pixel work on fewer pixels (≈60 fps at
900 px; native 1440 ≈ 32 fps — CPU JPEG-limited, the Orin Nano has no HW encoder).
INTER_AREA averaging removes hot pixels, so no median filter is needed.

## Auto-exposure (AE)

Meters the frame mean, drives the sensor over i2c (the driver's v4l2 controls are
the production interface). Control law: filter the metric (EMA), act in the
log-brightness domain with damping + a per-update slew cap and a wide deadband, and
spend **exposure first** (best SNR), gain only when exposure is maxed. This is
smooth and flicker-free near bright sources.

> Pitfall: do **not** use fixed additive exposure/gain steps — they overshoot the
> deadband near a light and pump (visible AE flicker).

## Duty cycle (for the NIR illuminator)

`duty = exposure_time / frame_time = exposure_lines / VMAX`, shown in the overlay.
At 60 fps in dim scenes the AE maxes exposure → duty ≈ 99% (shutter open ~16.5 ms of
the 16.67 ms frame). This is the window the NIR illuminator must fire within; it
only drops below ~99% when ambient light lets the AE shorten exposure.

## Preview tool features
Digital zoom 1/2/4/8×, native-1440 / 900-px toggle, live overlay (fps, mean, exp ms,
duty %, gain, FOV), and a `/stats` JSON endpoint (AE trace). FOV is computed from the
CommonLands CIL122 (f = 12 mm) on the IMX296 (3.45 µm px): **23.4° × 17.8°** at
1440×1088, scaling with zoom.

> Pitfall: no per-frame **row de-band**. Measured raw row-FPN is ~0.5 LSB
> (negligible). A content-derived row correction can't tell a real horizontal edge
> from row noise, so it bleeds the horizon/tree line into moving horizontal streaks
> (up to ~150 LSB) that track the scene. The 1440 sensor ROI keeps the line clean at
> the source — leave it. If a future sensor shows real row FPN, use a **static
> dark-frame per-row offset** (calibrated once), never a per-frame subtraction.

## Picture-quality status

The image is clean and sharp: no comb, no shear, no banding; row-FPN ~0.5 LSB,
temporal noise ~0.35 LSB. For a 10-bit mono global-shutter sensor the software
pipeline is close to optimal — the remaining levers are mostly **physical**:

- **Light / lens** — a faster lens or more scene light lets the AE drop gain (less
  noise) and shorten exposure (freeze motion). This is where the **NIR illuminator**
  pays off.
- **Exposure-vs-motion** — at 60 fps, exposure caps at 16.5 ms; a fast, close, or
  high-contrast target blurs. Shorten exposure (+ NIR fill) to freeze it.
- **Software headroom (small)** — a light edge-preserving denoise and a one-time
  flat-field/black calibration could give a modest gain; not currently limiting.

## Production hot path (C++/CUDA on the Orin Nano Super)

The Python tool is validated and runs the full ISP at ~30–60 fps. The shipping
version moves the datapath to C++/CUDA on the same Orin Nano Super (per the
guidelines): V4L2 mmap capture → CUDA unpack (Y10→16-bit) → black-level + tone/gamma
(LUT) → AE (histogram reduction → PID on exposure/gain) → zero-copy NVMM → detector.
Mono ⇒ no demosaic, no AWB. Budget: a few hundred µs/frame on the GPU, leaving the
16.6 ms frame for detection.

Note: the AE loop itself runs at a few Hz and a mono detector consumes the near-raw
linear Y10, so this port is mostly mechanical, not new design — much of the "ISP" is
optional for detection (cosmetic tone-mapping is for the human preview). No video
encode in the datapath (the target has no NVENC; see [STREAMING](STREAMING.md)).
