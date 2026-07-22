# EO Pipeline — the on-device EO module (`libeo`) + operator preview

The production EO datapath for the IMX296 camera, in C, on the Jetson Orin Nano Super.
It is split into two parts:

- **`libeo`** (`eo.h` + `libeo.a`) — **the module.** Owns the camera and the V4L2
  capture → auto-exposure loop, and hands the **raw full-native Y10** frame behind a
  **frozen** API. Consumers (the detector, a recorder, the preview) link it and pull the
  raw frame; they never open the camera or run AE. Internals change freely behind
  `eo.h`. **This is the contract — see [`INTEGRATION.md`](INTEGRATION.md).**
- **`eo_pipeline`** — a thin **operator preview** that links `libeo`, pulls the raw frame
  with `eo_latest()`, and **tone-maps + downscales it to the selected display size** (the
  work scales with what's shown, not the sensor), serving MJPEG + a control page on
  `:8091`. It's the bench/LAN tool and (for now) the thing the GUI proxies.

```
libeo (owns the camera, single V4L2 owner) — ONE thread:
  capture (~operating fps):
    V4L2 mmap (capture.c) ─► copy off uncached DMA ─► detector hook (consume_frame, raw Y10)
                         └─► AE + focus every 4th (ae.c ─► sensor i2c) ─► publish RAW ─► triple buffer
                                                                              │  eo_latest()  (raw, zero-copy)
  ────────────────────────────────────────────────────────────────────────────┼──────────────────────────
  eo_pipeline (preview):
    producer:  crop(zoom,4:3) + downscale + tone-map to the display size, ONE pass (isp.c)
    encode pool (3 workers):  NEON median ─► JPEG (libjpeg-turbo NEON) ─► publish IN ORDER
    serving:   event-driven — every published frame is written to every /stream client
  SG-IR850 illuminator (illum.c → ../../illuminator/src) ◄─ /ctl
```

**The 60 fps guarantee.** Sensor 60 → published 60 → **client receives 60**, at every
display size with median on — verified by counting frames a client actually receives,
not by a meter. What it took (each was a real, found bug): frames round-robin across a
3-worker encode pool (one thread can't JPEG native in <16.7 ms) with an in-order publish
gate; the serving loop is condvar-driven (the old 2 ms poll + latest-wins skipped frames
when workers published in bursts — clients saw ~half rate); the scaler's per-pixel
division and per-frame mallocs are gone; median runs in NEON (16 px/instruction).
`/stats` exposes raw pipeline counters (`prod`/`drop`/`pub`) so a stall shows *where*
frames vanish, and `fps` is **count-based** over a ≥0.5 s window — inter-arrival EMAs
are burst-biased and lied twice; a count cannot.

| File | Role |
|---|---|
| **`eo.h`** | **FROZEN** module API v1 (`eo_start/eo_stop/eo_latest/eo_connected/eo_focal_mm/eo_pixel_um`) |
| **`libeo.c`** | the module core — one capture thread, AE, raw triple-buffered Y10 frame |
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
kills frame-to-frame "breathing") + gamma; the 10→8 quantization is LUT-interpolated +
**ordered-dithered** (a ~6× night stretch of clean data contours if truncated), and
`isp_median3` is a cheap 3×3 median grain filter. The **detector always gets the raw
linear frame**, never this tone-mapped one.

## Night temporal denoiser (`tdn.c`) — display-only

Night frames are read-noise-limited (gain rails; ~0.7 LSB temporal noise gets stretched
~6× into grain + row banding). `tdn.c` runs a **motion-adaptive temporal IIR on the raw
native Y10 before the tonemap** — measured ~3.3× noise cut and the banding averages out.
Design points (each red-team-mandated; rationale in [`../docs/NIGHT_IQ.md`](../docs/NIGHT_IQ.md)):
- **4×4 block-pooled motion test** so a faint coherent mover trips the test and stays
  crisp (a per-pixel diff is provably blind to a far slow walker);
- AE steps **scale** the accumulator by the applied exposure×gain ratio (never reset),
  using the +2-frame register-landing model (`eo_frame_ae`);
- empirical per-intensity-bin noise scale (tracks read+shot noise at the applied gain);
- error-feedback accumulation (no fixed-point dead-band), row offsets against the
  accumulator reference (rows without static pixels get **zero** correction);
- global-motion guard (slew ⇒ pass-through + reseed) — a **static-mount instrument**,
  interim by contract until a warp-accumulation design exists for the tracking gimbal;
- **night-gated with hysteresis** on applied gain: day = zero cost, byte-identical path.

**Display-only by verdict:** temporal denoise attenuates exactly the faint slow movers
the detector exists to catch, so the detector keeps the raw tap and its own median-
background mover head — this block never feeds detection. Operator knob:
`/ctl?denoise=0|1` (`/stats`: `denoise`, `dn_active`, `dn_ms`). Offline validation:
`make tdn_bench` replays a recorded `eo_y10` channel through the exact shipped code.

## Two taps, one guarantee

- **Display tap** — finished 8-bit, shrunk to the operator's chosen size for bandwidth.
- **Detector tap** — the full-native **1440×1088** frame, inside `libeo`, **never
  reduced** by any display/bandwidth knob.

Shrinking the operator view never costs a target.

## Recorder taps (module outputs — protocol per `recorder/docs/TAP.md` v1)

Published via the vendored `airpoc_tap.h` (shm slot rings, publisher never blocks).
If the recorder/shm is absent, `tap_create` fails once at start and the pipeline runs
**exactly as before** (heap buffers) — the fallback is part of the contract.

| tap | slots × payload | what | `t_src_ns` | `meta[6]` |
|---|---|---|---|---|
| `airpoc.eo_y10` | 16 × `sizeimage` (3,133,440) | **raw pre-ISP native Y10**, every captured frame — the algorithm-replay stream. Zero added copies: the capture thread's one DMA memcpy lands directly in the tap slot; detector/AE/`eo_latest` read that same slot. | V4L2 buffer timestamp (CLOCK_MONOTONIC, exposure-referenced) | `{v4l2_seq, exp_lines, gain, vmax, illum, drops_cum}` — `illum` = `on \| present<<1 \| power<<8 \| (fov°×10)<<16` (recorder gates on `"illum":1` in `meta_json`); a `v4l2_seq` gap = driver drop; `drops_cum` accumulates them |
| `airpoc.eo_jpeg` | 16 × 1 MiB | display JPEG **exactly as encoded** (replay serves these bytes verbatim, incl. mid-session res changes; 1 MiB slot covers noisy NATIVE-mode night frames) | `tap_now_ns()` at publish | `{seq, dw, dh, zoom, res_idx, 0}` |

## Controls (preview `/ctl`, proxied by the GUI)

Full contract in [`INTEGRATION.md`](INTEGRATION.md). Two bandwidth levers —
`?res=low|med|high|native` (320×240 panic / 480×360 / **640×480 boot default** /
1440×1080 — a weak-link ladder, all **4:3**) and `?fps=12..60` — plus the whole ISP
panel (`ae, gain, expms, gaincap, median, zoom`) and the illuminator
(`laser, power, fov`). `/stats` reports every live value + the `prod/drop/pub`
pipeline counters.

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev
make                 # -> libeo.a (the module) + eo_pipeline (the preview)
make libeo.a         # just the linkable module (no consumer today — see INTEGRATION.md)
./eo_pipeline -d /dev/video0 -p 8091 -i /dev/ttyUSB0   # -i = illuminator port (optional)
```
**How it actually runs in production (verified on the Jetson, 2026-07-17):** the
`:8088` **launcher** starts it, via `app/launcher/start.sh` — *not* systemd.
`eo-pipeline.service` ships in this directory but is **NOT installed** on the box
(`systemctl is-enabled eo-pipeline` → no such unit). `start.sh` is a one-shot
idempotent starter: it health-checks each producer (port bound **and** shm tap
present), `setsid`-detaches the ones that are down, and exits. **It has no heal or
respawn loop** — it only re-runs on the launcher's `/start`, `/reattach` or `/resume`.

> **Consequence — there is no automatic restart today.** If `eo_pipeline` exits (see
> *Camera-drop behaviour* below), nothing brings it back until someone triggers a start
> from `:8088`. Radar and the detector share this exposure. A launcher heal loop (or
> installing the systemd unit instead) is the open fix; do not assume auto-recovery.

Restart EO **only** (leaves radar/detector/console alone):
```bash
cd ~/AIRPOC && git pull && cd eo/pipeline && make
pkill -x eo_pipeline                        # start.sh matches by exact name
( setsid ./eo_pipeline -d /dev/video0 -p 8091 -i /dev/ttyUSB0 >/tmp/airpoc-8091.log 2>&1 </dev/null & )
sudo -n systemctl restart airpoc-recorder   # re-attach recorder to the fresh shm taps
```
Note `</dev/null`: with a live stdin the process exits on EOF ("shutting down").
Logs: `/tmp/airpoc-8091.log`.

## Design notes
- **Copy the frame out of the V4L2 mmap before processing.** The DMA buffer is
  *uncached*: per-pixel work read straight from it is ~100× slower (measured 414 ms vs
  3.6 ms for the tone map). The capture thread does one streaming `memcpy` into a cached
  buffer and requeues immediately.
- **Process at the display resolution, not full native.** libeo hands the raw frame;
  the preview tone-maps + downscales it straight to the selected display size in one
  `isp_scale_tonemap` pass. Tone-mapping full native and *then* shrinking wasted ~2×
  the CPU (measured). `eo_latest()` is triple-buffered and zero-copy so a consumer's pull
  rate can never throttle capture or the detector.
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
- **Wire the real detector into `consume_frame()`** — the hook already receives every
  raw full-native Y10 frame inside libeo (and `eo_latest()` hands the same raw frame to
  a linked consumer), so the detector plugs in without any datapath change.
- **Camera-drop behaviour** — a dequeue failure or a stalled sensor (no frame within
  `CAP_DQ_TIMEOUT_MS`, `capture.c`) makes the process log, clear `connected`, and
  `_Exit(1)`. It deliberately does **not** limp on the last frame while `/stats` still
  claims `connected:1` — that silently froze the feed forever and left the detector
  chewing a stale frame. **Open item:** nothing auto-restarts it (see the run section —
  no systemd, launcher has no heal loop), so today this is an *honest hard-down*, not
  self-healing. Fix is either a launcher heal loop (covers radar + detector too) or an
  in-process reopen-retry.

## Status
> `-Wall -Wextra` clean; verified on the Orin Nano Super by **counting client-received
> frames**: 60 fps end-to-end (sensor → publish → client) at all four display sizes with
> median on. CPU (median on): 320p ~1.05 · 480p ~1.1 · 640p ~1.2 · native ~1.7 cores —
> the JPEG encode dominates and already runs libjpeg-turbo's internal NEON. AUTO holds
> the fixed fps with expose-first/low-gain; illuminator + focus + zoom live. GUI consumes
> via the frozen `eo.h` or by proxying MJPEG (`INTEGRATION.md`).
