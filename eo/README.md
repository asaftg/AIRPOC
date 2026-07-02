# EO Camera Module

The electro-optical camera subsystem: a **Waveshare IMX296-130** (Sony IMX296, mono
global shutter) on the Jetson, streaming **Y10 mono 1440×1088 @ 60 fps** with working
exposure/gain, a focus tool, a quality preview, and an MJPEG stream. Global shutter
means no rolling-shutter skew — the whole frame is exposed at once, which suits
fast-moving targets.

The on-device production datapath (capture + AE + ISP, in C) is
[`eo/pipeline/`](pipeline/README.md); the Python tools are bench/diagnostic.

Detail docs: [DRIVER](docs/DRIVER.md) · [IMAGE_PIPELINE](docs/IMAGE_PIPELINE.md) ·
[STREAMING](docs/STREAMING.md) · [FOCUS](docs/FOCUS.md).

## Architecture

```
 IMX296 sensor (54 MHz on-board xtal, global shutter, mono, ROI-cropped to 1440 wide)
        │  MIPI CSI-2, 1 lane
        ▼
 Tegra NVCSI ─► VI5 (nv_imx296 advertises Y10) ─► /dev/video0   [Y10, 60 fps]
        │
        ├─► eo/pipeline/   PRODUCTION C: capture + AE + ISP → detector + MJPEG :8091
        ├─► preview tool   eo/tools/imx296_preview.py   (bench: i2c AE + ISP → :8091)
        ├─► focus tool     eo/tools/imx296_focus_web.py (bench: sharpness → :8090)
        └─► MJPEG stream   eo/streaming/                (bench: low-latency UDP/RTP)
```

| Layer | Implementation | State |
|---|---|---|
| Sensor driver | `nv_imx296` tegracam C driver (`eo/driver/`) | ✅ Y10 60 fps, exposure/gain |
| DT overlay | `...imx296-C.dtbo` (CAM1 / serial_c) | ✅ |
| Capture | V4L2 `/dev/video0`, mmap | ✅ sustained 60 fps (verified) |
| AE + ISP | production C datapath (`eo/pipeline/`), same law as bench | ✅ production C (`eo/pipeline/`), on-device 60 fps + AE verified |
| Encode/stream | software MJPEG | ✅ target has no HW video encoder; detector consumes frames on-device |

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

## Run — the preview (production)

The shipping datapath **and** the operator's live view is [`eo/pipeline/`](pipeline/README.md).
Build once on the Jetson and run it:
```bash
cd eo/pipeline && make
./eo_pipeline -d /dev/video0 -p 8091 -i /dev/ttyUSB0   # -i = illuminator (optional)
```
Open **`http://<ip>:8091/`** (or `192.168.55.1:8091` over USB-C): live video with a
stats overlay (fps/exposure/duty/gain/FOV), **digital zoom 1–8×**, a **focus** assist
(target box + peak-% — turn the M12 ring to maximize), and, if attached, **illuminator
controls** (laser on/off, beam power, beam FOV). Full UI + endpoints:
[`pipeline/README.md`](pipeline/README.md).

### Bench tools (Python — diagnostic only, not shipped)
```bash
bash eo/tools/preview.sh          # AE/ISP preview  :8091   (superseded by eo/pipeline)
bash eo/tools/focus.sh            # standalone focus assist :8090
bash eo/streaming/imx296_stream.sh <your-ip> 5000   # low-latency MJPEG over UDP/RTP
```
Detail: [IMAGE_PIPELINE](docs/IMAGE_PIPELINE.md) · [FOCUS](docs/FOCUS.md) · [STREAMING](docs/STREAMING.md).
