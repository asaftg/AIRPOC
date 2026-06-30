# AIRPOC — Jetson Orin Nano + IMX296 Camera System

Production camera bring-up: **Waveshare IMX296-130** (Sony IMX296, mono global
shutter) on the **Jetson Orin Nano Super** (P3768 / P3767-0005), JetPack 6.2.2 /
L4T r36.4.4. This repo is the **single source of truth** — clones pull, never
hand-edit (`docs/ENGINEERING_GUIDELINES.md`).

## Status

| Item | State |
|---|---|
| `nv_imx296` driver | ✅ streams **Y10 1456×1088 @ 60 fps**, working exposure(µs)+gain v4l2 controls |
| Capture rate | ✅ sustained ~60 fps verified |
| AE + ISP (de-band, black-level, tone) | ✅ in the Python quality tool; C++/CUDA hot-path spec'd |
| Live quality preview (browser) | ✅ ~30 fps, de-banded, clean |
| High-fps stream | ✅ ~58 fps MJPEG (software); HW H.264 needs Xavier AGX (Orin Nano has no NVENC) |
| Fan | ✅ pinned 100% |
| IR illuminator (SG-IR850-8M) | ✅ C controller + `sgctl` CLI: on/off, power, FOV/zoom, status |
| Overlay clock modeling | ⚠️ one minor `fixed-clock` cleanup pending (functionally fine) |

## Quickstart

```bash
# live quality preview (focus + inspect), browser:
ssh asaftg@orin-nano 'bash ~/preview.sh'      # http://<ip>:8091
# focus assist:
ssh asaftg@orin-nano 'bash ~/focus.sh'        # http://<ip>:8090
# high-fps MJPEG stream (ffplay/VLC):
ssh asaftg@orin-nano 'bash ~/imx296_stream.sh <your-ip> 5000'
```

## Documentation

| Doc | Contents |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | system dataflow, components, hardware constraints |
| [`docs/DRIVER.md`](docs/DRIVER.md) | `nv_imx296` internals, register map, the 5 bring-up root causes, build |
| [`docs/IMAGE_PIPELINE.md`](docs/IMAGE_PIPELINE.md) | ISP (de-band/black-level/tone), AE, the C++/CUDA production hot path |
| [`docs/STREAMING.md`](docs/STREAMING.md) | measured fps, no-NVENC reality, bench vs Xavier streams |
| [`docs/NIR_SYNC.md`](docs/NIR_SYNC.md) | IMX296↔NIR sync design (self-clock ⇒ use trigger lines) |
| [`docs/JETSON_ORIN_NANO_BRINGUP.md`](docs/JETSON_ORIN_NANO_BRINGUP.md) | fresh-board → streaming checklist |
| [`docs/ENGINEERING_GUIDELINES.md`](docs/ENGINEERING_GUIDELINES.md) | C/C++ on device, Python for tools, no loose patches |
| [`docs/FOCUS_TOOL_GUIDE.md`](docs/FOCUS_TOOL_GUIDE.md) | focus assist usage |
| [`docs/ILLUMINATOR.md`](docs/ILLUMINATOR.md) | SG-IR850-8M IR illuminator: wiring, protocol, build, `sgctl` usage |

## Layout

```
jetson/camera/     nv_imx296 driver, mode tables, DT overlay  (C — production)
jetson/tools/      focus + quality/AE preview                 (Python — bench)
jetson/streaming/  high-fps MJPEG stream                      (Python+GStreamer)
jetson/fan/        always-100% fan service
jetson/illuminator/ SG-IR850-8M IR illuminator controller     (C — production)
docs/              the documentation set above
```

## The one-paragraph story

The camera looked unfixable for a long time — VI timeouts, then dark
uncontrollable images. None of it was the hardware. It was, in order: the wrong
driver bound (a prebuilt with dead controls), a 54 MHz on-board crystal the
overlay didn't know about, an `SHS1` underflow from control-apply ordering, the
MIPI byte-rate used where the sensor pixel clock was needed, and an RG10-vs-Y10
label. Fixed in our own clean driver; full details in `docs/DRIVER.md`.
