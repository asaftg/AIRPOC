# EO Streaming & FPS

How the EO feed is delivered and what frame rates are achievable on the Orin Nano.

## Rates (this hardware)

| Stage | Rate | Notes |
|---|---|---|
| Sensor + driver capture | **60.00 fps** | Y10 1440×1088, sustained (verified via raw v4l2 dequeue) |
| Preview (full ISP), 900 px | **~58–60 fps** | event-paced display loop; native 1440 ≈ 32 fps (CPU JPEG) |
| Software MJPEG (`jpegenc`) | **~58 fps** | low-latency bench stream, no ISP |
| Software H.264 (`x264enc`) | ~6 fps | not viable — CPU can't encode 1.6 MP at rate |

## Platform note: no hardware video encoder

> The Jetson Orin Nano's video encoder (**NVENC**) is fused off —
> `gst-inspect-1.0 nvv4l2h264enc` is MISSING. So there is **no HW H.264 on the
> target**: an encoded video feed is **software MJPEG** (light, higher bandwidth).
> In the field the detector/tracker consumes frames on-device, so an encoded stream
> is mainly an operator/bench convenience — size the link for MJPEG, not H.264.

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

## Production feed
On-device, frames go to the detector/tracker directly (no encode needed). For an
operator/monitor feed, use **software MJPEG** over RTP/UDP as above. The image
processing that runs before the detector is the C++/CUDA path in
[IMAGE_PIPELINE](IMAGE_PIPELINE.md).
