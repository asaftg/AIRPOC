# EO module integration (libeo) — the frozen handoff

The EO module owns the camera and the whole capture → auto-exposure → ISP datapath.
Consumers (the GUI, a recorder, a detector front-end) **do not** open `/dev/video0`,
run AE, drive the sensor, or tone-map. They link `libeo.a`, include `eo.h`, and pull
finished frames. Everything behind `eo.h` changes freely without touching consumers.

## The contract (`eo.h`, `EO_API_VERSION 1` — frozen)

```c
int    eo_start(const char *dev);   // open camera + run capture+AE+ISP. 0 ok, <0 fail
void   eo_stop(void);
int    eo_latest(const uint8_t **buf, uint64_t *seq,
                 int *w, int *h, int *stride, int *fmt);  // 1 if a frame, else 0
int    eo_connected(void);
double eo_focal_mm(void);
double eo_pixel_um(void);
```

- **`eo_latest`** hands the newest **raw, full-native Y10** frame (`fmt == EO_FMT_Y10`:
  linear 10-bit in a 16-bit LE word, read a pixel as `(p[2*x] | p[2*x+1]<<8) >> 6`),
  at **1440×1088**, `stride` bytes/row. The **detector** uses it directly; **display**
  consumers tone-map + downscale it to their chosen size themselves (see the preview,
  `main.c`) — the module doesn't tone-map 1.5M pixels just to shrink them. The pointer is
  **stable until your next `eo_latest()` on the same thread** — triple-buffered, **no
  copy**. `*seq` is a monotonic frame id; unchanged means no new frame yet.
- Any out-param may be `NULL`. Returns `0` (not an error) while the camera warms up.
- Frozen: if this surface ever must change, `EO_API_VERSION` bumps and this doc says so.

### Minimal consumer

```c
#include "eo.h"
if (eo_start("/dev/video0") < 0) { /* camera busy / no sensor */ }
const uint8_t *buf; uint64_t seq, last = 0; int w, h, stride, fmt;
for (;;) {
    if (eo_latest(&buf, &seq, &w, &h, &stride, &fmt) && seq != last) {
        last = seq;
        // buf = raw Y10 (2 B/px, >>6 for the value), valid until the next eo_latest()
    }
}
eo_stop();
```

Build: `make libeo.a`, then link `libeo.a -ljpeg -lpthread -lm` (libeo itself needs
`-lpthread -lm`; `-ljpeg` only if you also use the preview). Include `eo.h`.

## Camera ownership — single-owner V4L2

`/dev/video0` allows exactly **one** opener. So exactly one process may `eo_start()`
at a time:

- **Production: `eo_pipeline`.** It owns the camera and serves the module's HTTP
  surface on `:8091`. It is started by the **`:8088` launcher** (`app/launcher/start.sh`),
  **not** systemd — `eo-pipeline.service` ships in the repo but is not installed, and
  `start.sh` has no respawn loop, so **nothing auto-restarts EO today** (see README). The
  operator console (`app/`) does **not** link `libeo` — it is a thin HTTP proxy
  that consumes `:8091` (`app/Makefile` links its own five objects, no libeo and
  no libjpeg).
- **`libeo.a` is the static library `eo_pipeline` is built from** (capture, sensor,
  AE, ISP) and has **no consumer today**. Note what it does *not* contain: the
  `/ctl`, `/stats` and `/stream` HTTP surface documented below, the display loop that
  drives the ISP (`main.c`), the night denoiser (`tdn.c`) and the illuminator
  (`illum.c`) — all `APP_SRC`, i.e. the binary. What IS in `libeo.a` (`LIBEO_SRC` =
  `libeo.c capture.c sensor.c ae.c isp.c`): capture, sensor, AE, the ISP primitives
  (tone-map, median, metering) **and the full `eo_bench.h` control API** — every
  `eo_set_*` / `eo_stats` / `eo_frame_ae` is implemented in `libeo.c`. So linking
  `libeo.a` gets you frames *plus* the control API; what you do **not** get is the HTTP
  transport, the display loop, the denoiser and the illuminator.
- Only one process may hold the camera. `eo_start()` returns `<0` if it is already
  held, so stop the service before running a second instance by hand.

## What's behind the surface (may change any time — do not depend on it)

Auto-exposure ("expose don't gain": lengthen exposure, then add gain up to the cap —
the frame rate is operator-set and the AE never changes it), the p1/p99 tone-map,
the 3×3 median, sensor registers, thread layout.
Bench/preview controls (manual gain/exposure sweep, telemetry) are in `eo_bench.h`,
which is **explicitly unstable** and for the preview only — production consumers must
not include it.

## Alternative: consume the MJPEG feed instead

If you'd rather not link C at all, run the EO preview (`eo_pipeline`, which owns the
camera) and proxy its feed — same model as the radar SSE previewer:

| Endpoint (`:8091`) | Payload |
|---|---|
| `/stream` | `multipart/x-mixed-replace` MJPEG, finished frames at the selected size |
| `/stats`  | JSON telemetry (below) |
| `/ctl?...` | controls (below) |

Then the EO process owns the camera and you do zero capture/AE/ISP/encode. Pick one
model (link `libeo` **or** proxy MJPEG) — not both, because of single-owner V4L2.

## GUI control contract (`/ctl` — proxy these to the operator)

Two bandwidth levers + the full ISP panel. The **display** shrinks freely; the
**detector always runs on the full-native 1440×1088 frame**, untouched by any of this.

**Bandwidth levers**
| `/ctl` | Values | Effect |
|---|---|---|
| `?res=` | `low`(320×240 panic) · `med`(480×360) · `high`(640×480, **boot default**) · `native`(1440×1080) | display size — all **4:3**. Weak-link ladder: three tight low tiers hold a feed on a marginal downlink; `native` for USB/strong link |
| `?fps=` | 12…60 | **fixed** operating fps — caps exposure AND the stream rate |

> **`res` at zoom — it's a bandwidth lever, not always a detail lever.** The feed
> **crops the native sensor first, then scales the crop to `res`** (the correct order —
> it keeps every real pixel). At 1× the whole 1440-wide sensor is in view, so `res` also
> sets detail. But digital zoom shrinks the sensor slice: at N× the crop is only
> `1440/N` wide (~180 px at 8×). Once the crop is narrower than the chosen `res`, the
> extra output pixels are pure upscaling — same detail, more bytes. So at high zoom every
> `res` shows the **same detail** and differs only in bandwidth. That's correct and
> desirable on a weak link: **zoom in → drop `res` (free bandwidth, zero detail lost).**
> Do **not** ask for scale-then-crop to make the buttons "look different" — that would
> discard real detail and force higher bandwidth for the same picture. `/stats` exposes
> **`eff_w`/`eff_h`** = the real detail = `min(res, sensor-crop)` so the GUI can show the
> operator what they're actually getting.

**ISP panel** (everything on the previewer)
| `/ctl` | Values | |
|---|---|---|
| `?ae=` | 0/1 | auto-exposure on/off |
| `?gain=` | 0…480 | manual gain (0.1 dB/step); drops to manual |
| `?expms=` | ms | manual exposure (capped by fps) |
| `?gaincap=` | 0…480 | AE gain ceiling |
| `?median=` | 0/1 | 3×3 denoise |
| `?denoise=` | 0/1 | **night temporal denoiser** (display-only; default 1). The knob ENABLES it; it actually runs only when the night gate engages (high applied gain, hysteresis) — `/stats.dn_active` says so. Turn OFF to reclaim its CPU (~0.5 core at night); day cost is zero either way. Detector is unaffected (raw tap) |
| `?spot_ae=` | 0/1 | **illuminated-region AE** (default 1). When the illuminator is ON, the AE meters the beam region (auto-located; median of lit pixels, robust to blown road signs) instead of the whole frame — exposes the lit SUBJECT to mid-gray + cuts gain (less grain) instead of over-exposing it to lift a black background. Gated on illuminator-on + enough lit content, so daytime / far-dark falls back to whole-frame. `/stats.spot_active` says when it's running; `spot_cx/cy` = the beam-window center. Affects the whole pipeline (it's real exposure), so the detector benefits too |
| `?disp_fps=` | 12…60 | **display-only frame-rate cap** (default 30). Decimates the display chain (denoise → tone-map → encode) to `min(sensor_fps, disp_fps)`; the sensor rate, AE, exposure, and the detector's native Y10 tap all stay at full 60 fps. Halves preview CPU at 30. Wire rate of `/stream` = this value. |
| `?q=` | 30…95 | **JPEG quality** of the display stream (default 85, clamped). The *fine* bandwidth axis — the `res` ladder is a coarse 4-step cliff, this degrades gracefully. Measured on the wire: q85 ≈ 6.9 Mb/s, q60 ≈ 3.65 Mb/s (−47 %), q40 ≈ 2.6 Mb/s. On a weak link the same bytes buy ~640×480 at q60 instead of 320×240 at q85. Display path ONLY — the detector reads raw Y10 off the tap and the recorder's **native** channel records raw frames, so neither is affected (the recorder's *display* channel does follow it, being the preview copy). Below ~30 the 8×8 blocking starts eating small far-target signatures; above 95 costs bits for nothing |
| `?zoom=` | 1/2/4/8 | digital zoom |
| `?laser=`·`?power=`·`?fov=` | 0/1 · 0…255 · deg | illuminator |

> **Consumer gate:** the display chain is skipped entirely when **no `/stream` client is
> connected** (`/stats.clients == 0`) — no frames produced for nobody. In production the
> console holds a persistent `/stream` client, so this only idles the preview when truly
> nothing is watching. Consequence: the **`eo_jpeg`** recorder tap follows the served
> display (it's "the JPEGs the operator saw"), so it pauses when no viewer is connected;
> the **`eo_y10`** native tap is upstream and always records at full 60 fps, and native
> replay reconstructs the display from it — so nothing algorithm-facing is lost.

**`/stats`** returns all live values so the GUI renders the current state of every knob:
`fps` (**the emitted wire rate, count-based** over a ≥0.5 s window — every published
frame is also written to every `/stream` client, so this IS what a client receives on a
healthy link), `sfps` (sensor rate), **`fps_cap`** (the configured operating-fps cap =
the value set via `?fps=`; the AE never changes fps, so this is the set value, not a
guess), **`prod, drop, pub`** (raw pipeline counters: frames produced / skipped
pool-full / published — diff two polls to localize any stall), `res, dw, dh` (display
size), **`eff_w, eff_h`** (real detail = `min(res, sensor-crop)` — equals `dw×dh` at 1×,
collapses to the crop at high zoom), `mean, exp_ms, duty_pct, gain, gaincap, ae, median,
zoom, hfov, vfov, sharp, connected, laser, lpower, lfov, lpresent`, the denoiser
state: **`denoise`** (the knob), **`dn_active`** (night gate actually running this
frame), **`dn_ms`** (measured cost/frame — the GUI can surface it next to the toggle),
the display-rate state: **`disp_fps`** (applied cap) + **`clients`** (live `/stream`
clients; 0 = display chain idle), **`q`** (applied JPEG quality, 30…95 — read back after
`/ctl?q=` since it is clamped), and the spot-AE state: **`spot_ae`** (the knob),
**`spot_active`** (metering the beam this tick), **`spot_cx/spot_cy`** (auto-located beam
window center, native px; −1 = not yet located).

> Codec note: `/stream` is MJPEG (LAN/bench). For the RF datalink the same `res`/`fps`
> knobs apply to an H.264/RTSP output — added when the datalink is locked.

> **Recorder/replay note (tonemap v2):** the display tonemap now quantizes 10→8 via an
> interpolated LUT + static ordered dither, and at night the frame it maps is the
> temporally-denoised one (`tdn.c`, when `dn_active=1`). The recorder's byte-identical
> replay copy (`recorder/src/eo_tonemap.c`) will report `tonemap_vs_eo: drift` on new
> recordings until it vendored the v2 path — its drift guard working as designed, not
> data loss (`eo_y10` raw is unchanged; `eo_jpeg` stays byte-verbatim).

## Recorder taps (module outputs)

Two shm tap rings, protocol per `recorder/docs/TAP.md` v1 (publisher never blocks;
absent recorder → clean fallback, behavior unchanged):

- **`airpoc.eo_y10`** — raw pre-ISP native Y10, every captured frame, 16 slots ×
  `sizeimage`. `t_src_ns` = V4L2 exposure-referenced timestamp.
  `meta = {v4l2_seq, exp_lines, gain, vmax, illum, drops_cum}` where
  `illum = on | present<<1 | power<<8 | (fov°×10)<<16` — per-frame illuminator state;
  the recorder treats `meta[4]` as illuminator only when `meta_json` carries `"illum":1`
  (so pre-flag recordings stay correct). `mean10` moved to the 5 Hz `/stats` snapshot.
- **`airpoc.eo_jpeg`** — the display JPEG verbatim, 16 slots × 1 MiB (covers noisy
  NATIVE-mode night frames past 512 KiB). `meta = {seq, dw, dh, zoom, res_idx, 0}`
  (per-frame `dw/dh` so replay tracks mid-session res changes).
