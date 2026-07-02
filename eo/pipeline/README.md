# EO Pipeline — on-device capture + AE + ISP + preview (production C)

The production datapath for the EO camera, in C, on the Jetson Orin Nano Super. It
replaces the Python bench preview on the device: V4L2 mmap capture of the IMX296 Y10
stream, a flicker-free auto-exposure loop, a light mono ISP, a browser **operator
monitor** (MJPEG + controls), and a detector hook. AE/tone behaviour is identical to
the validated bench tool (`eo/tools/imx296_preview.py`) — a port to C, not a redesign.

It runs **two decoupled threads** so the wire feed can never throttle capture or the
detector. Capture runs at the full sensor rate on native Y10; the encoder runs
rate-capped (`EO_FEED_FPS`) and does the only heavy pixel work (tone-map + JPEG at
**full native resolution** — no downscale). This is the EO output the GUI consumes:
full-detail image, kept light on CPU/WiFi by capping the encode rate and JPEG quality
rather than by throwing away pixels — at all times, whether or not anyone watches.

```
capture thread (~60 fps, native full-res):
  V4L2 mmap (capture.c) ─► copy out of uncached DMA ─► AE meter+control every 4th (ae.c ─► sensor i2c sensor.c)
                       │                            └─► consume_frame() ─► detector (linear 10-bit; stub)
                       └─► publish newest Y10 + stats ─► framestore (mutex/cond)
                                                              │
encoder thread (rate-capped, EO_FEED_FPS):                   ▼
  wait for frame ─► crop(zoom)+tonemap at native res, one pass (isp.c) ─► MJPEG :8091 (mjpeg.c)
                └─► focus sharpness on native ROI
 SG-IR850 illuminator (illum.c → ../../illuminator/src) ◄─ monitor buttons (/ctl)
```

| File | Role |
|---|---|
| `capture.c` | V4L2 mmap capture; geometry probed from the driver |
| `sensor.c` | IMX296 exposure(SHS1)/gain over i2c, REGHOLD-latched |
| `ae.c` | flicker-free AE law (EMA + log-domain damped, exposure-first) |
| `isp.c` | Y10 metering + focus sharpness (native); `isp_scale_tonemap` = crop(zoom)+tone-map at native resolution |
| `mjpeg.c` | preview: HTML page + MJPEG + `/stats` + `/ctl` (libjpeg-turbo) |
| `illum.c` / `illum.h` | thread-safe shim over the SG-IR850 illuminator (optional device) |
| `main.c` | capture thread + encoder thread + framestore hand-off; `consume_frame()` is the detector hook |

The illuminator controller itself lives in `illuminator/src/` and is linked in
(`sg_ir850.o`); the Makefile references it via `../../illuminator/src`.

## Preview (`http://<ip>:8091/`)

A single HTML page over the MJPEG stream with a live overlay and control bar:

- **Overlay** (bottom-left, polled from `/stats` at ~7 Hz): fps, mean, exposure ms,
  duty %, gain, camera FOV, zoom — plus a **focus** readout and a red **LASER ON**
  line when firing.
- **Digital zoom** 1× / 2× / 4× / 8× (center crop; capture stays at the sensor rate).
- **Focus** button: shows a center target box + a sharpness value with peak-%
  (turn the M12 ring to maximize) — reads the native frame, correct at any zoom.
- **Illuminator** (if attached): **LASER ON/OFF** (ON confirms — invisible 850 nm
  laser), **beam pow −/+** (drive 0–255), **beam fov −/+** (motor zoom 1.96–70°, 1°
  steps under 25° for matching the ~23.4° camera FOV).

## EO output contract (what the GUI consumes)

The GUI is **only the GUI** — it does zero image processing. It consumes the already-
optimized feed from this module over HTTP. Everything below is served on `:8091`:

| Path | Purpose |
|---|---|
| `/` | the operator preview page (self-contained; for bring-up/testing, not the GUI) |
| `/stream` | **the feed** — `multipart/x-mixed-replace` MJPEG, encode-once-serve-many, full native resolution (zoom-cropped), tone-mapped. Just `<img src>` it. |
| `/stats` | JSON telemetry (poll ~5–7 Hz): `fps, mean, exp_ms, duty_pct, gain, zoom, hfov, vfov, sharp, laser, lpower, lfov, lpresent` |
| `/ctl?zoom=N` | digital zoom 1/2/4/8 (center crop) |
| `/ctl?laser=0\|1` · `power=0..255` · `fov=<deg>` | illuminator control |

**Wire load.** The feed is **full native resolution** (1440×1088, or the zoom crop) —
kept light without sacrificing image quality by capping the encode rate (`EO_FEED_FPS`,
default 25; capture and detection still run at ~60 fps) and the JPEG quality
(`EO_FEED_QUALITY`, default 85). On a detailed scene that lands around ~25 Mb/s; tune
those two constants for a tighter WiFi budget rather than downscaling — a 1.6 MP sensor
is already modest and small-target detail is what matters for counter-UAS.

Detection is unaffected — the detector always sees the **native full-res Y10** frame
inside this process (`consume_frame`). The `/stream` MJPEG is grayscale (mono sensor);
dimensions are reported live in `/stats`.

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev
make
./eo_pipeline -d /dev/video0 -p 8091 -i /dev/ttyUSB0   # -i = illuminator port (optional)
```

## Design notes
- **Two threads, decoupled through a mutex/cond framestore.** The capture thread runs
  at the sensor rate on native Y10 (dqbuf → memcpy → requeue → detector → AE) and never
  waits on the encoder. It publishes the newest frame + AE stats every 2nd frame; the
  encoder thread wakes, rate-caps to `EO_FEED_FPS` (dropping frames it's ahead of), and
  does the only heavy pixel work on a private copy. Result: the feed can't slow capture
  or detection, and its cost is fixed and small whether or not anyone is watching (no
  client-gating). Bound the wire budget with `EO_FEED_FPS`/`EO_FEED_QUALITY`, not by
  reducing resolution.
- **The encoder does one pass.** `isp_scale_tonemap` crops (the digital zoom) and
  tone-maps to 8-bit at native resolution in a single loop — no separate full-res
  buffer built and discarded.
- **Copy the frame out of the V4L2 mmap before processing.** The DMA buffer is
  *uncached*: per-pixel work read straight from it is latency-bound, ~100× slower
  (measured 414 ms vs 3.6 ms for the tone map). The capture thread does one streaming
  `memcpy` into a cached buffer and requeues immediately; all work runs on the copy.
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
> **`-Wall -Wextra` clean.** Prior single-thread build verified on-device (Orin Nano
> Super): 60 fps, AE converged garage↔sun, zoom, focus assist, and the illuminator
> detected + FOV/power/on-off controllable from the page. Illuminator integration
> per [`illuminator/docs/PREVIEW_INTEGRATION.md`](../../illuminator/docs/PREVIEW_INTEGRATION.md).
>
> **This revision:** capture/encoder split into two threads; the feed is **full native
> resolution** (no downscale), kept light by capping the encode rate (`EO_FEED_FPS`) and
> JPEG quality (`EO_FEED_QUALITY`). Capture/detection stay at ~60 fps while the encoder
> runs capped; encode-once-serve-many so extra clients are nearly free. Remaining to
> fully retire the Python preview: a systemd unit, and wiring the real detector into
> `consume_frame()`.
