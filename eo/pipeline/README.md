# EO Pipeline — on-device capture + AE + ISP + operator monitor (production C)

The production datapath for the EO camera, in C, on the Jetson Orin Nano Super. It
replaces the Python bench preview on the device: V4L2 mmap capture of the IMX296 Y10
stream, a flicker-free auto-exposure loop, a light mono ISP, a browser **operator
monitor** (MJPEG + controls), and a detector hook. AE/tone behaviour is identical to
the validated bench tool (`eo/tools/imx296_preview.py`) — a port to C, not a redesign.

```
V4L2 mmap (capture.c) ─► copy out of uncached DMA ─► AE meter+control (ae.c) ─► sensor i2c (sensor.c)
                     │                            └─► ISP Y10→8-bit (isp.c) ─► zoom ─► MJPEG monitor :8091 (mjpeg.c)
                     └─► consume_frame() ─────────────────────────────────────► detector (linear 10-bit; stub)
 SG-IR850 illuminator (illum.c → ../../illuminator/src) ◄─ monitor buttons (/ctl)
```

| File | Role |
|---|---|
| `capture.c` | V4L2 mmap capture; geometry probed from the driver |
| `sensor.c` | IMX296 exposure(SHS1)/gain over i2c, REGHOLD-latched |
| `ae.c` | flicker-free AE law (EMA + log-domain damped, exposure-first) |
| `isp.c` | Y10 unpack, metering, tone map, digital zoom, focus sharpness |
| `mjpeg.c` | operator monitor: HTML page + MJPEG + `/stats` + `/ctl` (libjpeg-turbo) |
| `illum.c` / `illum.h` | thread-safe shim over the SG-IR850 illuminator (optional device) |
| `main.c` | wiring; `consume_frame()` is the detector hook |

The illuminator controller itself lives in `illuminator/src/` and is linked in
(`sg_ir850.o`); the Makefile references it via `../../illuminator/src`.

## Operator monitor (`http://<ip>:8091/`)

A single HTML page over the MJPEG stream with a live overlay and control bar:

- **Overlay** (bottom-left, polled from `/stats` at ~7 Hz): fps, mean, exposure ms,
  duty %, gain, camera FOV, zoom — plus a **focus** readout and a red **LASER ON**
  line when firing.
- **Digital zoom** 1× / 2× / 4× / 8× (center crop, stays 60 fps).
- **Focus** button: shows a center target box + a sharpness value with peak-%
  (turn the M12 ring to maximize) — reads the native frame, correct at any zoom.
- **Illuminator** (if attached): **LASER ON/OFF** (ON confirms — invisible 850 nm
  laser), **beam pow −/+** (drive 0–255), **beam fov −/+** (motor zoom 1.96–70°, 1°
  steps under 25° for matching the ~23.4° camera FOV).

### HTTP endpoints
| Path | Purpose |
|---|---|
| `/` | the operator page |
| `/stream` | MJPEG multipart |
| `/stats` | JSON: fps, mean, exp_ms, duty_pct, gain, zoom, hfov, vfov, sharp, laser, lpower, lfov, lpresent |
| `/ctl?zoom=N` | digital zoom 1/2/4/8 |
| `/ctl?laser=0\|1` · `power=0..255` · `fov=<deg>` | illuminator control |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev
make
./eo_pipeline -d /dev/video0 -p 8091 -i /dev/ttyUSB0   # -i = illuminator port (optional)
```

## Design notes
- **Copy the frame out of the V4L2 mmap before processing.** The DMA buffer is
  *uncached*: per-pixel work read straight from it is latency-bound, ~100× slower
  (measured 414 ms vs 3.6 ms for the tone map). `main.c` does one streaming `memcpy`
  into a cached buffer and requeues immediately; all processing runs on the copy.
- **AE** runs at ~15 Hz (every 4th frame); light, doesn't gate capture. Exposure
  range 0.074–16.5 ms (`SHS1` 8..1120) covers garage-dark to direct sun.
- **Sensor control is i2c** (SHS1 `0x308d`, GAIN `0x3204`, REGHOLD `0x3008`) — the
  path proven on this board. Switch to `VIDIOC_S_EXT_CTRLS` once the driver's v4l2
  exposure control is verified end-to-end.
- **Illuminator is optional and off the hot path.** `illum_start()` no-ops if the
  device is absent. Serial writes happen only on the `/ctl` client threads, **never
  the capture loop** (they block a few ms). The single handle is mutex-guarded. The
  device forces full drive on every laser-on, so the shim re-applies the commanded
  power after each ON. Beam FOV is the *light beam* angle — distinct from camera FOV.
- **The detector consumes linear Y10** (`consume_frame`); tone/gamma is for the human
  monitor only. **No video encode in the datapath** — the target has no NVENC; the
  monitor uses software MJPEG (see [../docs/STREAMING.md](../docs/STREAMING.md)).

## Status
> **Verified on-device (Orin Nano Super, `-Wall -Wextra` clean):** 60 fps, AE
> converged garage↔sun, zoom holds 60 fps, focus assist, and the illuminator
> detected + FOV/power/on-off controllable from the page. Illuminator integration
> per [`illuminator/docs/GUI_INTEGRATION.md`](../../illuminator/docs/GUI_INTEGRATION.md).
>
> Remaining to fully retire the Python preview from the device: wrap in a systemd
> unit, and wire the real detector into `consume_frame()`.
