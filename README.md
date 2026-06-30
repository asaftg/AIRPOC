# AIRPOC — Jetson Orin Nano + IMX296 camera

Bench: **Jetson Orin Nano Super** (JetPack 6.2.2 / L4T r36.4.4, kernel
`5.15.148-tegra`) + **Waveshare IMX296-130** mono global-shutter MIPI camera,
headless. This repo is the **single source of truth** — clones pull, never hand-edit.

## Docs
- [`docs/JETSON_ORIN_NANO_BRINGUP.md`](docs/JETSON_ORIN_NANO_BRINGUP.md) — bring-up **checklist** (fresh board → focused, auto-exposed stream).
- [`docs/ENGINEERING_GUIDELINES.md`](docs/ENGINEERING_GUIDELINES.md) — code/build rules (C/C++ on device, Python for tools only, no loose patches).
- [`docs/FOCUS_TOOL_GUIDE.md`](docs/FOCUS_TOOL_GUIDE.md) — focus-assist tool.

## Layout
- `jetson/camera/` — production `nv_imx296` driver, mode tables, DT overlay.
- `jetson/fan/` — fan-always-100% service.
- `jetson/tools/` — bench tools (Python OK): focus + quality/AE preview.

## Status
Streams **Y10 mono 1456×1088 @ 60 fps**. Root cause of the long bring-up:
**stock Tegra VI silently drops mono Y10** — not the clock, ribbon, or hardware.

**Exposure/gain** is driven via the sensor registers **SHS1 `0x308d`**
(`SHS1 = VMAX − exposure_lines`) and **GAIN `0x3204`** (0–480). A clean
production driver exposing these as working v4l2 controls is the remaining
camera task — the diagnostic prebuilt accepts control values but never writes
them to the sensor. Y10 is **left-justified** (shift `>>6` for the raw value).
