# EO Streaming & FPS

How the EO feed is delivered and what frame rates are achievable on the Orin Nano.

## Rates (this hardware)

| Stage | Rate | Notes |
|---|---|---|
| Sensor + driver capture | **60.00 fps** | Y10 1440×1088, sustained (verified via raw v4l2 dequeue) |
| Preview (full ISP), 900 px | **~58–60 fps** | event-paced display loop; native 1440 ≈ 32 fps (CPU JPEG) |
| Software MJPEG (`jpegenc`) | **~58 fps** | low-latency bench stream, no ISP |
| Software H.264 (`x264enc`) | ~6 fps | not viable — CPU can't encode 1.6 MP at rate |
| C++/CUDA ISP + HW H.264 | 60 fps | **Xavier AGX** target (has NVENC) |

## Platform note: no hardware encoder on the Orin Nano

> The Jetson Orin Nano's video encoder (**NVENC**) is fused off —
> `gst-inspect-1.0 nvv4l2h264enc` is MISSING. On the bench, encoding is **software**
> (MJPEG is light, H.264 is too heavy). The production seeker uses **Xavier AGX**,
> which has NVENC; the zero-copy NVMM → HW-H.264 pipeline lives there.

> Also: GStreamer `v4l2src` cannot negotiate Y10, so pipelines are fed via `appsrc`
> from a V4L2 reader, not `v4l2src`.

## How to stream

**Browser MJPEG preview** (AE + ISP + overlay) — `eo/tools/imx296_preview.py`:
```bash
bash eo/tools/preview.sh          # http://<ip>:8091
```
Best over the **USB-C link (`192.168.55.1`)** — wired, sub-ms latency, no WiFi
contention (a ~25–30 Mbps MJPEG stream saturates a weak WiFi link).

**Low-latency MJPEG over RTP/UDP** — `eo/streaming/imx296_stream.sh`:
```bash
bash eo/streaming/imx296_stream.sh <your-ip> 5000
ffplay -fflags nobuffer -flags low_delay -protocol_whitelist file,udp,rtp rtp_mjpeg.sdp
```

## Production stream (Xavier AGX)
Capture (V4L2) → CUDA ISP (NVMM) → `nvv4l2h264enc` → `rtph264pay` → RTSP/UDP.
Zero-copy, 60 fps, full ISP — what the Orin Nano can't do for lack of NVENC. CUDA
stages: [IMAGE_PIPELINE](IMAGE_PIPELINE.md).
