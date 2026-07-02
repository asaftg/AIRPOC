# Radar → GUI integration

For the GUI agent. The radar daemon is standalone and publishes over HTTP on
**:8092**; couple to it through this contract only (never its internals).

## Endpoints

| Method | Path | Returns |
|---|---|---|
| GET | `/` | the standalone PPI previewer (works on its own) |
| GET | `/radar_view.js` | the previewer script (reference renderer) |
| GET | `/stream` | **SSE** — one `data: <frame-json>\n\n` per radar frame |
| GET | `/stats` | `{fps, drops, num_points, num_targets, connected, profile, max_range_m, fov_half_deg, cluster_eps_m, cluster_min_pts}` |
| GET | `/ctl?eps=<m>&minpts=<int>` | set the host DBSCAN live → `200 ok` |

## Live controls — `/ctl` (CLUSTER ε + MIN PTS sliders)

`GET /ctl?eps=<metres>&minpts=<int>` sets the host-side DBSCAN **live** (no
restart, applies on the next frame) and returns `200 ok`. Both params are
optional — an absent one keeps its current value.

- `eps` — cluster spacing in metres, clamped to **0.5 – 50** (default 8).
- `minpts` — DBSCAN min-samples, clamped to **1 – 20** (default 2).

`/stats` echoes the currently-applied `cluster_eps_m` and `cluster_min_pts`
(post-clamp), so initialise the sliders from `/stats` on load rather than
assuming defaults. Tighter `eps` / higher `minpts` = fewer, tighter boxes;
looser = more merging. Changes take effect on the **next frame** — a box that
stops clustering disappears immediately (no coasting).

## Frame JSON (the `/stream` payload)

```json
{ "connected": true, "frame_id": 60, "timestamp": 1248.807,
  "profile": "awr2944P_ag.cfg", "max_range_m": 500.0, "fov_half_deg": 90.0,
  "num_points": 18, "num_targets": 2,
  "points":  [{"x":-11.9,"y":30.5,"z":0.1,"v":0.45,"snr":null,"r":32.7,"az":-21.4,"el":0.2,"tid":1}],
  "targets": [{"tid":1,"x":-11.6,"y":30.2,"z":0.1,"vx":1.34,"vy":0.25,"vz":0.04,
               "sx":0.95,"sy":0.99,"sz":0.25,"conf":1.0,"np":61,
               "class":"radar_detection"}] }
```

- **Frame:** sensor frame, metres. `+x` right, `+y` forward (boresight), `+z`
  up. `az = atan2(x,y)` deg, `el = atan2(z, hypot(x,y))` deg.
- **points[]:** raw cloud. `v` = radial Doppler m/s (**+approaching**). `snr`
  is **`null`** on the current firmware (no SideInfo TLV). `tid` = owning
  track, or `255` if unclustered (static clutter, gated out at 0.4 m/s).
- **`fov_half_deg`:** the cfg's full azimuth span (±90) — the chip publishes the
  whole span so the GUI can trim it live. **The GUI owns two filters:** an
  azimuth slider (works today — every point has `az`) and an SNR slider
  (**inert until Phase-2 firmware**, since `snr` is `null` now). Default the
  azimuth slider to ~±60 (useful AoA) and grey out the SNR slider while every
  `snr` is `null`.
- **targets[]:** **class-less per-frame detections** — a box is emitted **only
  for a cluster detected this frame**. There is **no coasting**: a target the
  sensor doesn't see this frame simply isn't in the list (it does not linger or
  dead-reckon). `sx/sy/sz` are box half-extents; `vx/vy/vz` a light per-frame
  velocity estimate; `conf` 0..1. `tid` is stable across frames via nearest-
  neighbour association (so boxes keep their colour), but is a transient per-
  sensor id — **fusion assigns the global id**. `class` is always
  `"radar_detection"` — labelling is fusion's.
  > Display persistence is the **GUI's** job — if you don't want a box to blink
  > on a one-frame miss, hold-and-fade it briefly on your side (~300 ms). Real
  > motion-model coasting belongs to the future **tracking** module, not here.

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
