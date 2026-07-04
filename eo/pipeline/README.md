# EO Pipeline — the on-device EO module (`libeo`) + operator preview

The production EO datapath for the IMX296 camera, in C, on the Jetson Orin Nano Super.
It is split into two parts:

- **`libeo`** (`eo.h` + `libeo.a`) — **the module.** Owns the camera and the whole
  V4L2 capture → auto-exposure → ISP path, and produces finished, display-ready frames
  behind a **frozen** API. Consumers (the GUI, a recorder, the future detector) link it
  and pull frames; they never open the camera or run AE/ISP. Internals change freely
  behind `eo.h`. **This is the contract — see [`INTEGRATION.md`](INTEGRATION.md).**
- **`eo_pipeline`** — a thin **operator preview** that links `libeo`, pulls frames with
  `eo_latest()`, applies digital zoom + the selected display size, and serves them as
  MJPEG with a control page on `:8091`. It is the bench/LAN tool and (for now) the thing
  the GUI proxies.

```
libeo (owns the camera, single V4L2 owner):
  capture thread (~sensor fps):
    V4L2 mmap (capture.c) ─► copy off uncached DMA ─► detector hook (consume_frame, full Y10)
                         └─► AE every 4th (ae.c ─► sensor i2c sensor.c) ─► publish raw ─► framestore
  tone thread (at the operating fps):
    tone-map + 3×3 median on the full native frame (isp.c) ─► finished 8-bit, triple-buffered
                                                                    │  eo_latest()  (zero-copy)
  ────────────────────────────────────────────────────────────────┼───────────────────────────
  eo_pipeline (preview) or GUI:  zoom + 4:3 crop + area-scale to the selected size ─► MJPEG :8091
  SG-IR850 illuminator (illum.c → ../../illuminator/src) ◄─ /ctl
```

| File | Role |
|---|---|
| **`eo.h`** | **FROZEN** module API v1 (`eo_start/eo_stop/eo_latest/eo_connected/eo_focal_mm/eo_pixel_um`) |
| **`libeo.c`** | the module core — capture + tone threads, AE, ISP orchestration, triple-buffered finished frame |
| `eo_bench.h` | **unstable** bench controls (manual gain/exposure sweep, telemetry) — preview only, not for the GUI |
| `capture.c` | V4L2 mmap capture; geometry probed from the driver |
| `sensor.c` | IMX296 VMAX(fps) + exposure(SHS1) + gain over i2c, REGHOLD-latched together |
| `ae.c` | auto-exposure: "expose don't gain" at a **fixed** fps (below) |
| `isp.c` | Y10 metering + focus (native); smoothed p1/p99 tone-map; `isp_median3` grain filter |
| `mjpeg.c` | preview server — HTML page + MJPEG + `/stats` + `/ctl` (display size, fps, full ISP panel, illuminator) |
| `main.c` | preview: `eo_latest()` → zoom + 4:3 crop + area-scale to the selected size → publish |
| `illum.c`/`illum.h` | thread-safe shim over the SG-IR850 illuminator (optional) |

## Auto-exposure — "expose don't gain", at a FIXED fps

The night-grain fix and a hard operator requirement, both live here:

- **fps is a fixed operator setting** (`eo_set_fps`, 12–60). It caps the maximum
  exposure (max integration = frame period) **and** the AE never changes it — no frame
  dropping. More exposure headroom for a dark scene = the operator lowers fps, on
  purpose, not the AE behind their back.
- Within that fixed budget the AE spends **exposure first, then minimal gain** (gain
  hard-capped, `EO_GAIN_CAP`). It re-runs the ladder every tick, so gain is continuously
  minimized — it only rises once exposure is maxed for the chosen fps. (This is why the
  IMX296 night image went from 48 dB-of-gain grain to clean: the grain was the gain
  rail, not the sensor.)

## ISP — for the human view only

`isp_scale_tonemap` builds the finished 8-bit frame from the raw Y10 with a
**temporally-EMA-smoothed p1/p99 stretch** (p99 ignores a blown streetlight; smoothing
kills frame-to-frame "breathing") + gamma, and `isp_median3` is a cheap 3×3 median grain
filter. The **detector always gets the raw linear frame**, never this tone-mapped one.

## Two taps, one guarantee

- **Display tap** — finished 8-bit, shrunk to the operator's chosen size for bandwidth.
- **Detector tap** — the full-native **1440×1088** frame, inside `libeo`, **never
  reduced** by any display/bandwidth knob.

Shrinking the operator view never costs a target.

## Controls (preview `/ctl`, proxied by the GUI)

Full contract in [`INTEGRATION.md`](INTEGRATION.md). Two bandwidth levers —
`?res=low|med|high|native` (640×480 / **960×720 default** / 1280×960 / 1440×1080, all
**4:3**) and `?fps=12..60` — plus the whole ISP panel (`ae, gain, expms, gaincap,
median, zoom`) and the illuminator (`laser, power, fov`). `/stats` reports every live
value.

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev
make                 # -> libeo.a (the module) + eo_pipeline (the preview)
make libeo.a         # just the linkable module (what the GUI links)
./eo_pipeline -d /dev/video0 -p 8091 -i /dev/ttyUSB0   # -i = illuminator port (optional)
```
**Production run — as a service** (boot-persistent + auto-restart; the reliable way):
```bash
sudo cp eo-pipeline.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now eo-pipeline
journalctl -u eo-pipeline -f      # logs
```
Runs from this persistent clone (not `/tmp`) and `Restart=always` brings it back if the
single-owner camera is briefly grabbed — no stale binaries, no manual bounces.

## Design notes
- **Copy the frame out of the V4L2 mmap before processing.** The DMA buffer is
  *uncached*: per-pixel work read straight from it is ~100× slower (measured 414 ms vs
  3.6 ms for the tone map). The capture thread does one streaming `memcpy` into a cached
  buffer and requeues immediately.
- **Two threads in libeo**, decoupled through a mutex/cond framestore, so a consumer's
  pull rate can never throttle capture or the detector. `eo_latest()` is triple-buffered
  and zero-copy (pointer valid until the next call).
- **Sensor control is i2c** (VMAX `0x3010`, SHS1 `0x308d`, GAIN `0x3204`, REGHOLD
  `0x3008`), all latched together per frame.
- **Illuminator is optional and off the hot path** — serial writes happen only on the
  `/ctl` client threads, never the capture loop; the device resets to full drive on
  every laser-on, so the shim re-applies the commanded power.

## Future work
- **H.264/RTSP output for the RF datalink** (when the SIYI HM30/MK15 arrives). MJPEG is
  fine on the LAN but ~5–10× too fat for the radio; the datalink wants H.264/H.265. The
  Orin Nano Super has **no hardware encoder**, so this is software x264 (ultrafast,
  zerolatency) → RTSP on `192.168.144.x` for the air unit, **or** feed the Orin's HDMI
  to the SIYI HDMI→Ethernet converter (HW H.265, but 30 fps-capped). The same
  `res`/`fps` knobs drive it. See [../docs/STREAMING.md](../docs/STREAMING.md).
- **Detector raw tap** — an `eo_latest_raw()` handing the full-native **linear Y10**
  (the detector wants linear sensor data, not the human tone-map). ~2-line add; the data
  already flows inside libeo.
- **Camera-drop resilience in-process** — currently `Restart=always` (systemd) handles a
  dropped/held camera by restarting; a nicer refinement is an in-process reopen-retry so
  the process never exits. Low priority given the service already auto-recovers.

## Status
> `-Wall -Wextra` clean; `libeo.a` + `eo_pipeline` built and verified on the Orin Nano
> Super. AUTO holds a fixed fps with expose-first/low-gain; all four display sizes emit
> exact 4:3; the fps lever gates both exposure and stream rate; illuminator + focus +
> zoom live. GUI consumes via the frozen `eo.h` or by proxying MJPEG (`INTEGRATION.md`).
