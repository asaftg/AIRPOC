# EO Image Pipeline (ISP + AE)

How a raw Y10 frame becomes a clean, well-exposed image. The **shipping implementation
is the C module `libeo`** (`eo/pipeline/`); the Python tool (`eo/tools/`) was the
validated reference it was ported from. It runs on the Orin Nano Super's A78 cores —
CUDA was not needed; C at native res holds the frame budget.

## Pipeline (in `libeo`, per finished frame)

```
Y10 (>>6) → metrics on the native frame (AE mean, focus)
          → tone-map: EMA-smoothed p1/p99 stretch (raw 10-bit) → γ=0.85 LUT → 8-bit
          → 3×3 median (isp_median3, grain filter) → finished full-native frame
                                            │  detector gets the RAW linear frame here
  display path (preview/GUI): zoom + 4:3 crop + area-average scale to the selected size → MJPEG
```

The tone-map endpoints are the **1st/99th percentiles**, not min/max — p99 ignores a
blown streetlight, and both are **EMA-smoothed across frames** so the mapping doesn't
wobble (the "breathing" a per-frame stretch caused). The **3×3 median** is a cheap
edge-preserving grain filter. Downscale to the operator's display size is
**area-average** (also knocks noise down); the detector never sees any of this — it
gets the raw linear frame.

## Auto-exposure (AE) — "expose don't gain", at a FIXED fps

Meters the frame mean, drives VMAX+SHS1+GAIN over i2c (latched together per frame).
Control law: filter the metric (EMA), act in the log-brightness domain with damping +
a per-update slew cap and a wide deadband. Two hard rules:

- **fps is fixed** by the operator (`eo_set_fps`, 12–60). It caps the maximum exposure
  (max integration = frame period) and the AE **never changes it — no frame dropping.**
  Lowering fps for a dark scene is a deliberate operator choice, not an AE action.
- Within that fixed budget: spend **exposure first, then minimal gain** (hard-capped).
  The ladder re-runs every tick, so gain is continuously minimized — it only rises once
  exposure is maxed for the chosen fps. This is what took the night image from
  48 dB-of-gain grain to clean: **the grain was the gain rail, not the sensor.**

> Pitfall: do **not** use fixed additive exposure/gain steps — they overshoot the
> deadband near a light and pump (visible AE flicker). And do **not** let the AE drop
> fps to buy exposure — fps is an operator constant.

## Duty cycle (for the NIR illuminator)

`duty = exposure_time / frame_time = exposure_lines / VMAX`, shown in the overlay.
At 60 fps in dim scenes the AE maxes exposure → duty ≈ 99% (shutter open ~16.5 ms of
the 16.67 ms frame). This is the window the NIR illuminator must fire within; it
only drops below ~99% when ambient light lets the AE shorten exposure.

## Preview / control features
Digital zoom 1/2/4/8×; four operator-selectable **4:3** display sizes (640×480 /
960×720 / 1280×960 / 1440×1080); the full ISP panel (ae, gain, expms, gaincap, median,
fps) + illuminator, all over `/ctl`; live overlay + `/stats` JSON. FOV is computed from
the CommonLands **CIL122 (f = 12 mm, F/2.0)** on the IMX296 (3.45 µm px):
**23.4° × 17.8°** at 1440×1088, scaling with zoom. Full contract:
[../pipeline/INTEGRATION.md](../pipeline/INTEGRATION.md).

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

## Shipping datapath (C, on the Orin Nano Super)

Built and running as `libeo` (`eo/pipeline/`): two threads — capture (V4L2 mmap →
copy off uncached DMA → detector hook + AE) and tone (tone-map + median → finished
triple-buffered frame). Mono ⇒ no demosaic, no AWB. CUDA turned out unnecessary — C on
the A78 cores holds the frame budget; the whole finish is a small fraction of a core.
The **detector consumes the near-raw linear Y10** (cosmetic tone-map is for the human
view only). No video encode in the datapath (the target has no NVENC — encoding the
operator display for the RF link is software H.264, see [STREAMING](STREAMING.md)).
Module contract: [../pipeline/INTEGRATION.md](../pipeline/INTEGRATION.md).
