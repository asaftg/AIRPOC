# Streaming & FPS

What "highest fps" means on this hardware, and how to stream.

## The numbers (measured on this Orin Nano)

| Stage | Rate | Notes |
|---|---|---|
| Sensor + driver **capture** | **~60 fps** (58.6 measured, v4l2 reports 60.00) | Y10 1456×1088, sustained |
| Software **MJPEG** (`jpegenc`) | **~58 fps** (~21 KB/frame) | viable bench high-fps stream, no ISP |
| Software **H.264** (`x264enc` ultrafast) | **~6 fps** | NOT viable — CPU can't encode 1.6 MP at rate |
| Python **full-ISP preview** | ~30 fps | ISP-bound (de-band/tone in NumPy) |
| Production **C++/CUDA ISP + HW H.264** | 60 fps | Xavier AGX target (`IMAGE_PIPELINE.md`) |

So on the bench you choose **58 fps MJPEG (no ISP)** *or* **30 fps clean
(de-banded)**. 60 fps **with** the full ISP needs the GPU ISP path (and HW H.264
for an encoded stream needs the Xavier AGX).

## Hardware reality: no encoder on the Orin Nano

**The Jetson Orin Nano has no hardware video encoder (NVENC is fused off).**
`gst-inspect-1.0 nvv4l2h264enc` → MISSING. So on this bench, encoding is
**software** (`x264enc`, `jpegenc`, `avenc_mjpeg`). Software H.264 at 1456×1088@60
is CPU-heavy; MJPEG is lighter but high-bandwidth.

The production target **Xavier AGX has NVENC** — the zero-copy NVMM → HW-H.264
pipeline lives there, not here.

Also: **GStreamer `v4l2src` cannot negotiate Y10**, so GStreamer pipelines are
fed via `appsrc` from a V4L2 reader, not `v4l2src`.

## Bench streams

### A. Browser MJPEG preview (what you view today)
`jetson/tools/imx296_preview.py` — v4l2 AE + de-band ISP, MJPEG over HTTP.
```bash
ssh asaftg@orin-nano 'bash ~/preview.sh'     # http://<ip>:8091
```
Best over the USB-C link (`192.168.55.1`) when WiFi bandwidth limits MJPEG fps.

### B. ~58 fps MJPEG over RTP/UDP (low-latency, VLC/ffplay)
`jetson/streaming/imx296_stream.sh` — light reader (Y10→8-bit) → `jpegenc` →
`rtpjpegpay` → `udpsink`. Max-fps monitoring path (no de-band). View with:
```bash
ssh asaftg@orin-nano 'bash ~/imx296_stream.sh <your-ip> 5000'
ffplay -fflags nobuffer -flags low_delay -protocol_whitelist file,udp,rtp rtp_mjpeg.sdp
# or: gst-launch-1.0 udpsrc port=5000 caps="application/x-rtp,encoding-name=JPEG,payload=26" ! rtpjpegdepay ! jpegdec ! autovideosink
```
(Software H.264 was measured at ~6 fps — not usable; use MJPEG here, or the
Xavier AGX HW encoder for an H.264 stream.)

## Production stream (Xavier AGX)
Capture (V4L2) → CUDA ISP (de-band/AE/tone, NVMM) → `nvv4l2h264enc` →
`rtph264pay` → RTSP/UDP. Zero-copy, 60 fps, low latency, with full ISP — the
combination the Orin Nano can't do because it lacks NVENC. See `IMAGE_PIPELINE.md`
for the CUDA stages.
