# EO Camera Module

The electro-optical camera subsystem: a **Waveshare IMX296-130** (Sony IMX296, mono
global shutter) on the Jetson, streaming **Y10 mono 1440×1088 @ 60 fps** with working
exposure/gain, a focus tool, a quality preview, and an MJPEG stream. Global shutter
means no rolling-shutter skew — the whole frame is exposed at once, which suits
fast-moving targets.

Detail docs: [DRIVER](docs/DRIVER.md) · [IMAGE_PIPELINE](docs/IMAGE_PIPELINE.md) ·
[STREAMING](docs/STREAMING.md) · [FOCUS](docs/FOCUS.md).

## Architecture

```
 IMX296 sensor (54 MHz on-board xtal, global shutter, mono, ROI-cropped to 1440 wide)
        │  MIPI CSI-2, 1 lane
        ▼
 Tegra NVCSI ─► VI5 (nv_imx296 advertises Y10) ─► /dev/video0   [Y10, 60 fps]
        │
        ├─► preview tool   eo/tools/imx296_preview.py   (i2c AE + ISP → MJPEG :8091)
        ├─► focus tool     eo/tools/imx296_focus_web.py (sharpness metrics → MJPEG :8090)
        └─► MJPEG stream   eo/streaming/                (low-latency UDP/RTP)
```

| Layer | Implementation | State |
|---|---|---|
| Sensor driver | `nv_imx296` tegracam C driver (`eo/driver/`) | ✅ Y10 60 fps, exposure/gain |
| DT overlay | `...imx296-C.dtbo` (CAM1 / serial_c) | ✅ |
| Capture | V4L2 `/dev/video0`, mmap | ✅ sustained 60 fps (verified) |
| AE + ISP | black-level + adaptive-white tone + gamma | ✅ in Python tool; C++/CUDA hot-path is the production target |
| Encode/stream | software (Orin Nano has no NVENC) | ✅ MJPEG on bench; HW H.264 belongs on Xavier AGX |

## Key facts (read before changing anything)

- **Y10 is left-justified** in the 16-bit word: `>>6` for the 10-bit value, `>>8`
  for 8-bit. i2c addr `0x1a`; find the bus with `ls -d /sys/bus/i2c/devices/*-001a`.
- **The sensor self-clocks at 54 MHz** (its own crystal) — the Jetson mclk is
  ignored. Multi-camera / NIR genlock must use the sensor's trigger lines, not a
  shared Jetson clock.
- **Sensor is ROI-cropped to 1440 wide** (from 1456) so the Y10 line is 64-byte
  aligned — otherwise the Tegra VI interleaves odd/even lines into a "comb". See
  [DRIVER](docs/DRIVER.md).
- **Exposure control is `SHS1`** (`0x308d`): `SHS1 = VMAX − exposure_lines`, smaller
  = brighter. At 60 fps, exposure caps at ~16.5 ms (line_time 14.815 µs × up to 1117
  lines). Gain `0x3204`, 0–480 (0.1 dB/step).

## Bring-up (on the Jetson)

Prereq: platform ready per [`jetson/`](../jetson/README.md). Kernel modules are
kernel-specific, so **build on-device**.

1. Build `nv_imx296.ko` from `eo/driver/` and install to
   `/lib/modules/$(uname -r)/updates/`. Full build command + register map:
   [DRIVER](docs/DRIVER.md).
2. Install the overlay `tegra234-p3767-camera-p3768-imx296-C.dtbo` to `/boot/`,
   enable via `jetson-io`, reboot.
3. Verify:
   ```bash
   v4l2-ctl -d /dev/video0 --list-formats-ext        # Y10 1440x1088
   v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10 --stream-to=/tmp/c.raw
   ls -l /tmp/c.raw                                   # ≈31 MB (10 frames)
   ```
   0 bytes / `VI request timed out` ⇒ VI is dropping Y10 → recheck the driver's
   mono-Y10 advertisement (see [DRIVER](docs/DRIVER.md)).

## Run the tools

```bash
# quality preview (AE + ISP + zoom/duty/FOV overlay), browser:
bash eo/tools/preview.sh          # http://<ip>:8091   (or 192.168.55.1 over USB-C)
# focus assist (turn the M12 ring until the metrics peak):
bash eo/tools/focus.sh            # http://<ip>:8090
# low-latency MJPEG stream:
bash eo/streaming/imx296_stream.sh <your-ip> 5000
```
Preview features + the picture-quality status: [IMAGE_PIPELINE](docs/IMAGE_PIPELINE.md).
Focusing: [FOCUS](docs/FOCUS.md). Stream/fps: [STREAMING](docs/STREAMING.md).
