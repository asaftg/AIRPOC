# Radar → GUI integration

For the GUI agent. The radar daemon is standalone and publishes over HTTP on
**:8092**; couple to it through this contract only (never its internals).

## Endpoints

| Method | Path | Returns |
|---|---|---|
| GET | `/` | the standalone PPI previewer (works on its own) |
| GET | `/radar_view.js` | the previewer script (reference renderer) |
| GET | `/stream` | **SSE** — one `data: <frame-json>\n\n` per radar frame |
| GET | `/stats` | `{fps, drops, num_points, num_targets, connected, profile, max_range_m, cluster_eps_m, cluster_min_pts, speed_min_mps, snr_min_db, fov_half_deg, el_max_deg, doppler_gate_mps, confirm, coast_s, park_s, dsp_valid, dsp_proc_ms, dsp_margin_ms, active_cpu_pct, interframe_cpu_pct}` |
| GET | `/ctl?eps=&minpts=&speed=&snrmin=&fov=&elmax=&doppler=&confirm=&coast=&park=` | set the live tracker knobs → `200 ok` |

## Recorder taps (module outputs — protocol per `recorder/docs/TAP.md` v1)

Full-rate data the recorder consumes over **shared memory** (HTTP can't carry it
losslessly). One publisher (this daemon), overwrite-oldest ring, the daemon
**never blocks** on the recorder — one `memcpy` per read/frame. Vendored header:
`radar/tap/airpoc_tap.h`. Taps are best-effort: if shm creation fails the daemon
logs once and runs unchanged (a recorder-less system is identical).

| tap | slots × cap | payload | `meta[6]` |
|---|---|---|---|
| `airpoc.radar_raw` | 512 × 8 KiB | raw UART bytes **verbatim, before the parser** (both read sites) so capture is independent of parse health | — (none) |
| `airpoc.radar_wire` | 16 × 256 KiB | the `/stream` frame JSON **byte-verbatim** (replay serves these bytes straight back to the GUI) | `{frameNumber, n_points, n_targets, 0, 0, 0}` |

`t_src_ns` = `CLOCK_MONOTONIC` at the UART `read()` return (raw) / frame publish
(wire). Not part of the GUI contract — the GUI consumes `/stream`; these taps are
for the recorder only.

## Live controls — `/ctl` (the 10 tuning knobs)

`GET /ctl?...` sets the host-side tracker **live** (no restart, applies on
the next frame) and returns `200 ok`. Every param is optional — an absent one
keeps its current value. All are clamped server-side. Full meaning/rationale
per knob: [`TUNING.md`](TUNING.md).

| key | units | min | max | default | what it does |
|---|---|---|---|---|---|
| `eps` | m | 0.5 | 50 | 4.5 | dedup radius — co-located tracks emit one box |
| `minpts` | count | 1 | 20 | 2 | radar dots needed to start a track |
| `speed` | m/s | 0 | 5 | 0.7 | Doppler motion threshold (below = static clutter) |
| `snrmin` | dB | 0 | 60 | 16 | per-point SNR gate (chip CFAR already floors at ~16 dB) |
| `fov` | deg | 5 | 90 | 90 | azimuth half-angle gate, input **and** emit |
| `elmax` | deg | 5 | 90 | 20 | elevation half-angle gate (radar-frame, gimbal-safe; 90 = off) |
| `doppler` | m/s | 0.5 | 20 | 1.2 | merge gate — co-located tracks merge only if speeds agree within this |
| `confirm` | hits | 1 | 6 | 3 | M-of-N fast-confirm (lower = appears faster, more false) |
| `coast` | s | 0 | 3 | 0.4 | how long a confirmed track survives a dropout |
| `park` | s | 0 | 60 | 15 | how long a moved-then-stopped track is held |

`/stats` echoes all ten **post-clamp** under: `cluster_eps_m`,
`cluster_min_pts`, `speed_min_mps`, `snr_min_db`, `fov_half_deg`, `el_max_deg`,
`doppler_gate_mps`, `confirm`, `coast_s`, `park_s` — initialise the sliders
from `/stats` on load, don't assume defaults. Changes take effect on the
**next frame**.

## Chip DSP timing in `/stats` (frame-rate health)

`/stats` also reports the chip's own per-frame timing, parsed from the stats TLV
(type 6): `dsp_proc_ms` (DSP range/Doppler/CFAR/AoA time), `dsp_margin_ms`
(spare time before the next frame — **the frame-rate ceiling; must stay > 0**),
`active_cpu_pct`, `interframe_cpu_pct`, and `dsp_valid` (`false` until the first
stats TLV arrives, then `true`). Surface `dsp_margin_ms` as a health readout —
if it trends to ~0 the frame period is too aggressive and frames will drop.

## Frame JSON (the `/stream` payload)

```json
{ "connected": true, "frame_id": 60, "timestamp": 1248.807,
  "profile": "awr2944P_ag.cfg", "max_range_m": 500.0, "fov_half_deg": 90.0,
  "num_points": 18, "num_targets": 2,
  "points":  [{"x":-11.9,"y":30.5,"z":0.1,"v":0.45,"snr":42.0,"r":32.7,"az":-21.4,"el":0.2,"tid":1}],
  "targets": [{"tid":1,"x":-11.6,"y":30.2,"z":0.1,"vx":1.34,"vy":0.25,"vz":0.04,
               "sx":0.95,"sy":0.99,"sz":0.25,"conf":1.0,"np":61,
               "class":"radar_detection"}] }
```

- **Frame:** sensor frame, metres. `+x` right, `+y` forward (boresight), `+z`
  up. `az = atan2(x,y)` deg, `el = atan2(z, hypot(x,y))` deg.
- **points[]:** raw cloud. `v` = radial Doppler m/s (**+approaching**). `snr`
  is the **per-point SNR in dB** (live — typically ~16–50 dB, floored at the
  CFAR threshold). It can be `null` only if a firmware without SideInfo is ever
  flashed; the shipped A/G cfg emits it. `tid` = owning track, or `255` if
  unclustered (static clutter, gated out at 0.4 m/s).
- **`fov_half_deg`:** the cfg's full azimuth span (±90) — the chip publishes the
  whole span so the GUI can trim it live. **The GUI owns two filters, both
  live:** an azimuth slider (every point has `az`) and an **SNR slider** (every
  point has `snr` in dB — filter on it directly). Default the azimuth slider to
  ~±60 (useful AoA) and the SNR slider to the ~16 dB floor (raise to hide weak
  returns).
- **targets[]:** **class-less confirmed temporal tracks** — a box is emitted
  after the track passes M-of-N confirmation plus a post-confirm consistency
  guard (ghosts/wanderers are killed), **coasts briefly** through dropouts
  (`coast`), and a moved-then-stopped target is held (`park`). `sx/sy/sz` are
  box half-extents; `vx/vy/vz` velocity from the track's own position history;
  `conf` 0..1. `tid` is stable across frames, but is a transient per-sensor
  id — **fusion assigns the global id**. `class` is always
  `"radar_detection"` — labelling is fusion's.
  > The tracker confirms/coasts/park-holds, so the GUI does **not** add its
  > own display persistence — render the list as-is.

## Consuming it

Simplest — subscribe directly (what the previewer does):
```js
const es = new EventSource("http://<jetson>:8092/stream");
es.onmessage = (e) => render(JSON.parse(e.data));
```
`web/radar_view.js` is the reference PPI renderer (points, boxes,
velocity arrows, breathing range rings, FOV wedge) — reuse or adapt it. If the
GUI prefers to proxy rather than hit :8092 cross-origin, forward `/stream` and
`/stats` through the GUI's own server.

## Develop with the board off

`./radar_preview -s -w ../web` runs the whole pipeline against a **synthetic
scene** (walking person + receding vehicle + static clutter) — same parser,
clusterer, wire format, and endpoints as the radio. Build and test the GUI
integration entirely without the Jetson, then drop `-s` on the device.
