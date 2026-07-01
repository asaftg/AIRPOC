# EO Pipeline — on-device capture + AE + ISP (production C)

The production datapath for the EO camera, in C, running on the Jetson Orin Nano
Super. It replaces the Python bench preview on the device: V4L2 mmap capture of the
IMX296 Y10 stream, a flicker-free auto-exposure loop that drives the sensor over
i2c, a light mono ISP, and an MJPEG monitor feed. The **numeric behaviour is
identical to the validated bench tool** (`eo/tools/imx296_preview.py`) — same AE law,
same tone map — it is a port to C, not a redesign.

```
V4L2 mmap (capture.c) ─► AE metering + control (ae.c) ─► sensor i2c (sensor.c, REGHOLD)
                     └─► ISP Y10→8-bit (isp.c) ─► MJPEG monitor :8091 (mjpeg.c)
                     └─► consume_frame() ────────► detector  (linear 10-bit; stub)
```

| File | Role |
|---|---|
| `capture.c` | V4L2 mmap capture; geometry probed from the driver |
| `sensor.c` | IMX296 exposure(SHS1)/gain over i2c, REGHOLD-latched |
| `ae.c` | flicker-free AE law (EMA + log-domain damped, exposure-first) |
| `isp.c` | Y10 unpack, metering, black-level + adaptive-white tone + gamma |
| `mjpeg.c` | MJPEG-over-HTTP monitor (libjpeg-turbo) |
| `main.c` | wiring; `consume_frame()` is the detector hook |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev
make
./eo_pipeline -d /dev/video0 -p 8091      # monitor: http://<ip>:8091/ , stats: /stats
```

## Design notes
- **Copy the frame out of the V4L2 mmap before processing.** The DMA capture
  buffer is *uncached*: per-pixel work read straight from it is latency-bound and
  ~100× slower (measured 414 ms vs 3.6 ms for the tone map). `main.c` does one
  streaming `memcpy` into a cached buffer and requeues immediately; all processing
  runs on the copy. (This is why the bench tool copied `maps[i][:N]` too.)
- **AE** runs at ~15 Hz (every 4th frame); it is light and does not gate capture.
- **Sensor control is i2c** (SHS1 `0x308d`, GAIN `0x3204`, REGHOLD `0x3008`) — the
  path proven on this board. The driver's v4l2 exposure/gain controls are the
  cleaner interface; switch `sensor.c` to `VIDIOC_S_EXT_CTRLS` once that control is
  verified end-to-end on-device.
- **The detector consumes linear Y10** (`consume_frame`), not the tone-mapped 8-bit —
  cosmetic tone/gamma is for the human monitor only.
- **No video encode in the datapath** — the target has no NVENC; the monitor uses
  software MJPEG (see [../docs/STREAMING.md](../docs/STREAMING.md)).
- Single-threaded loop keeps up at 60 fps in C (ISP+encode ≈ a few ms). If headroom
  is ever needed, move JPEG encode to a worker or the ISP to CUDA/VPI.

## Status
> **Written; compiles cleanly** (no warnings under `-Wall -Wextra`, links
> libjpeg/pthread). **On-device run + verify pending** (the Jetson was offline when
> this landed). It is a direct C port of the validated bench logic. First on-device run:
> `make` on the board, point at `/dev/video0`, compare the `:8091` feed and `/stats`
> (mean ≈ 450, duty/exposure sane, no AE flicker) against the bench tool, then wire a
> systemd unit and retire the Python preview from the device.
