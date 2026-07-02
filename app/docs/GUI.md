# Operator GUI — layout, controls, endpoints, compute budget

The console is one self-contained web page (embedded in the binary) over the same
lightweight pattern as the EO bench monitor: **MJPEG `/stream` + polled `/stats` +
`/radar` + `GET /ctl`**. No websockets, no framework, no CDN (the operator laptop is
on the drone AP with no internet — text labels + inline SVG + canvas only).

## Layout
Full-viewport, no scroll. EO fills almost the whole screen; radar is a promotable
**PIP + swap** (limited-azimuth **sector**, not a 360° PPI). Minimal text, large touch
targets (14″ laptop → tablet). Visual language matches the Seeker ground bench:
amber `#ffb454` on near-black, dense monospace/tabular, cyan radar.

- **Top bar:** `FAZE-1`, link Mb/s, BATT·ALT (reserved), ZULU (client UTC), theme
  toggle, DEV. No SCAN/TRACK.
- **EO hero:** amber reticle, FOV/zoom (live), BRG/RNG (reserved), detection box
  (reserved), and the engaged-target **LOCK** (green — a projected box when the target
  is inside the EO FOV, else an edge arrow pointing to it).
- **Radar scope:** range rings, amber 250 m reference, cyan FOV wedge, doppler-coloured
  returns (red inbound / blue outbound / cyan static), clustered target boxes with
  velocity + trails; engaged target shown as a green LOCK.
- **Bottom cluster (big buttons):** `LIGHT` (laser on/off), `ILLUM` (auto/man),
  `TRACK` (auto/man), `REC` (reserved); zoom ± bottom-left.
- **DEV panel:** stream presets + fps/quality + est-Mb/s; illuminator PWR/BEAM (manual
  only); radar display filters + cluster cfg; system temp + EO source fps.

## Theme
Dark **night** default + **day** toggle (persisted in `localStorage`). Day is a bright
white/high-contrast theme for direct sun; the video and on-glass symbology (reticle,
scope, locks, overlay chips) stay vivid in both modes.

## Tracking — AUTO / MANUAL (cluster `TRACK`)
- **AUTO:** engages the most important target automatically — priority **fused
  (EO+radar) → nearer range → higher confidence**. (Fused pending the detector; on
  radar-only it is nearest, tie-broken by confidence.)
- **MANUAL:** engages the target you **tap** — on the EO (mapped to azimuth via the
  camera FOV) or directly on the radar scope. A hint banner shows until one is picked.
- The engaged tid flows to the server (`/ctl?engage=`) for the gimbal/effector later.

## Illuminator — AUTO / MANUAL (cluster `ILLUM`)
- **AUTO:** fits the beam FOV to the **camera FOV** at **max power**, re-applied on zoom
  (server-side). DEV PWR/BEAM sliders are disabled.
- **MANUAL:** PWR/BEAM come from the DEV sliders.
- `LIGHT` is the separate on/off fire control (confirm on ON; bright firing state).

## Radar source
The scope consumes the **`radar/` daemon** (radar/docs/INTEGRATION.md). `radar_client.c`
subscribes to its SSE `:8092/stream`, keeps the latest frame, and the app serves it
**verbatim on `/radar`** so the browser stays single-origin (never touches `:8092`).
Frame schema is the daemon's (points `{x,y,z,v,snr,r,az,el,tid}`, class-less targets
with `coasting`). `tid=255` = unclustered clutter. Develop with the board off via the
daemon's `-s` sim.

## Radar tuning (DEV) — two distinct groups
- **Display filters (client-side only):** `FOV ±`, `SPEED MIN`, `RANGE MIN` gate what
  the scope draws from the daemon's cloud. **`SNR` is inert** — the current firmware
  omits SNR (`snr:null`); the slider is disabled until Phase-2 fw.
- **Cluster cfg (→ radar daemon):** `CLUSTER ε` (DBSCAN spacing) + `MIN PTS`, sent via
  `/ctl` → `radar_set_tune()` → the daemon. **Needs a daemon `/ctl` endpoint** (request
  pending with the radar agent); a no-op until it lands.
- **Chip cfg is the radar module's job, not the GUI:** SNR floor **≥17 dB** and **max
  FOV**. The daemon publishes the full ±90 span; the GUI trims azimuth for display.

## HTTP endpoints
| Path | Purpose |
|---|---|
| `/` · `/app.css` · `/app.js` | the embedded page (served `no-cache` — assets change per build) |
| `/stream` | MJPEG multipart (the latest small picture, fanned to every screen) |
| `/stats` | JSON, polled ~6 Hz (below) |
| `/radar` | the latest radar-daemon frame **verbatim** (polled ~8 Hz): the daemon's schema — `connected, max_range_m, fov_half_deg, points:[{x,y,z,v,snr,r,az,el,tid}], targets:[{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,coasting,class}]`. Proxied from `:8092/stream`. |
| `/ctl?…` | one-shot controls (below) |

`/stats` fields: `fps` (encoder), `src_fps` (EO channel), `mbps`, `zoom`, `res_w/h`,
`fps_cap`, `q`, `preset`, `hfov`, `vfov`, `track` (auto/man), `illum_mode` (auto/man),
`engage` (tid or -1), `cpu_c`, `laser`, `lpower`, `lfov`, `lpresent`, `tracks`.
Reserved `null`: `cam_c`, `batt`, `alt`, `brg`, `rng`.

`/ctl` params: `zoom=1|2|4|8`, `preset=LEAN|SMOOTH|BALANCED|CLEAR`, `fps=1..60`,
`q=10..95`, `res=<width>`, `track=auto|man`, `engage=<tid|-1>`, `illum=auto|man`,
`laser=0|1`, `power=0..255` (manual), `fov=<deg>` (manual),
`radar_eps=<m>`, `radar_minpts=<n>`.

## Stream presets (fps-priority)
Native MJPEG saturates weak WiFi, so 60 fps is bought with small, lower-quality
frames. Detection stays native/full-res; only the display loses quality.

| Preset | Width | FPS | q | ≈ link |
|---|---|---|---|---|
| LEAN | 400 | 60 | 40 | ~1.5–3 Mb/s |
| SMOOTH (default) | 512 | 60 | 45 | ~5–8 Mb/s |
| BALANCED | 800 | 30 | 60 | ~4–6 Mb/s |
| CLEAR | 1152 | 25 | 75 | ~8–12 Mb/s |

Output height follows the source aspect. `est Mb/s` in DEV = jpeg_bytes × enc_fps × 8.

## Compute budget (edge device)
- **The GUI adds no load to the EO channel.** It only reads the latest frame
  (`eo_get_latest`, borrowed pointer + seq). No shrink/compress runs on the capture path.
- **One shrink+compress worker**, rate-capped, off the capture path: one pass
  (zoom-crop + box-downscale straight to a small buffer) then one libjpeg-turbo encode.
  It skips ticks when the source seq hasn't advanced.
- **Compress once, serve many.** A slow screen only skips to the newest picture; it
  can't back-pressure the worker or the camera.
- **Format:** software MJPEG only — the Orin Nano has no NVENC and no NVJPG *encode*
  (both fused off). At 60 fps this is ~one CPU core; pin it away from the EO channel and
  detector.

> Pitfall: no internet on the drone AP — the page must stay self-contained (no CDN
> fonts/icons). Text labels + inline SVG + canvas only.

> Pitfall: `LIGHT` is live-fire (invisible 850 nm, eye hazard). The ON path confirms
> and the button shows a bright firing state.
