# Architecture

End-to-end IMX296 camera system on the Jetson Orin Nano, and where it goes for
production.

## Dataflow

```
 IMX296 sensor (54 MHz xtal, global shutter, mono)
        │  MIPI CSI-2, 1 lane, 1188 Mbps
        ▼
 Tegra NVCSI ──► VI5 (tegra-camera.ko, Y10-capable) ──► /dev/video0  [Y10, 60 fps]
        │
        ├──► Quality preview tool (Python)      jetson/tools/imx296_preview.py
        │      v4l2 AE → de-band ISP → MJPEG/HTTP  (30 fps, for focus/inspection)
        │
        ├──► Focus tool (Python)                jetson/tools/imx296_focus_web.py
        │      Tenengrad/Laplacian → MJPEG/HTTP
        │
        └──► Production stream                  jetson/streaming/
               light ISP → software/HW encode → network  (see STREAMING.md)
```

## Components

| Layer | Implementation | Status |
|---|---|---|
| Sensor driver | `nv_imx296` Tegracam C driver | **done** — Y10 60 fps, exposure/gain |
| DT overlay | `...imx296-C.dtbo` (CAM1 / serial_c) | **done** (one minor clock-modeling cleanup pending) |
| Capture | V4L2 `/dev/video0`, mmap | **done** — sustained ~60 fps verified |
| AE | exposure(µs)+gain via v4l2 ctrls | **done** (in the Python tools) |
| ISP | black-level + row-noise de-band + tone | **done in Python**; C++/CUDA port is the production hot-path target |
| Encode/stream | software (Orin Nano has no NVENC) | bench: software; production: Xavier AGX HW |
| Fan | always-100% service | **done** |

## Hardware constraints (important)

- **Jetson Orin Nano has NO hardware video encoder (NVENC is fused off).**
  HW H.264/H.265 (`nvv4l2h264enc`) is unavailable. Bench streaming uses software
  (`x264enc`/`jpegenc`). The production target **Xavier AGX has NVENC** — the HW
  encode pipeline belongs there.
- **GStreamer `v4l2src` cannot negotiate Y10.** The production-format mono Y10
  isn't in GStreamer's v4l2 format map, so capture is done via V4L2 directly
  (the tools) or fed into GStreamer via `appsrc`. See `STREAMING.md`.
- **The sensor self-clocks (54 MHz).** Multi-camera / NIR genlock must use the
  sensor's trigger lines, not a shared Jetson clock. See `NIR_SYNC.md`.

## Production targets (per guidelines)

On-device hot-path code is **C/C++** (`ENGINEERING_GUIDELINES.md`). The Python
tools are bench/diagnostic utilities. The production image pipeline (capture →
de-band/AE/tone → encode) is specified for a C++/CUDA(VPI) implementation in
`IMAGE_PIPELINE.md`.

## Repo map

```
docs/                       this documentation set
jetson/camera/              nv_imx296 driver, mode tables, DT overlay
jetson/tools/               Python bench tools: focus, quality/AE preview
jetson/streaming/           production stream scripts (software encode on bench)
jetson/fan/                 always-100% fan service
```
