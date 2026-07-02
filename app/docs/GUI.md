# Operator GUI — endpoints, controls, compute budget

The console is one self-contained web page (embedded in the binary) over the same
lightweight pattern as the EO bench monitor: **MJPEG `/stream` + polled `/stats` +
`GET /ctl`**. No websockets, no framework, no CDN.

## Layout
Full-viewport, no scroll, dark **night** default + **day** toggle (persisted). EO
fills almost the whole screen; radar is a promotable **PIP + swap** (limited-azimuth
sector, not a 360° PPI). Minimal text, large touch targets (14″ laptop → tablet).

- Top bar: `FAZE-1`, SCAN/TRACK, link Mb/s, BATT·ALT (reserved), ZULU (client UTC),
  day/night, DEV.
- EO hero: reticle, FOV/zoom (live), BRG/RNG (reserved), detection box (reserved).
- Bottom cluster: LIGHT · AUTO-FOV (live), TRACK · REC (reserved), zoom ± (live).
- DEV: stream presets + fps/quality + est-Mb/s, illuminator power/beam, CPU temp.

## HTTP endpoints
| Path | Purpose |
|---|---|
| `/` · `/app.css` · `/app.js` | the embedded page |
| `/stream` | MJPEG multipart (the latest small picture, fanned to every screen) |
| `/stats` | JSON, polled ~6 Hz (below) |
| `/radar` | JSON radar frame, polled ~8 Hz: `connected`, `max_range_m`, `fov_half_deg`, `points:[[x,y,v,snr],…]`, `targets:[[tid,x,y,vx,vy,sx,sy,conf],…]` (metres, sensor frame). Source `radar_stub.c` until the AWR module lands. |
| `/ctl?…` | one-shot controls (below) |

`/stats` fields: `fps` (encoder), `src_fps` (EO channel), `mbps`, `zoom`, `res_w/h`,
`fps_cap`, `q`, `preset`, `hfov`, `vfov`, `mode`, `cpu_c`, `laser`, `lpower`, `lfov`,
`lpresent`. Reserved fields are `null`: `cam_c`, `batt`, `alt`, `brg`, `rng`, `tracks`.

`/ctl` params: `zoom=1|2|4|8`, `preset=LEAN|SMOOTH|BALANCED|CLEAR`, `fps=1..60`,
`q=10..95`, `res=<width>`, `mode=scan|track`, `laser=0|1`, `power=0..255`,
`fov=<deg>`, `autofov=1` (sets beam FOV to the current camera FOV).

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
  (`eo_get_latest`, borrowed pointer + seq). No shrink/compress runs on the capture
  path.
- **One shrink+compress worker**, rate-capped, off the capture path: one pass
  (zoom-crop + box-downscale straight to a small buffer) then one libjpeg-turbo
  encode. It skips ticks when the source seq hasn't advanced.
- **Compress once, serve many.** A slow screen only skips to the newest picture; it
  can't back-pressure the worker or the camera.
- **Format:** software MJPEG only — the Orin Nano has no NVENC and no NVJPG *encode*
  (both fused off). At 60 fps this is ~one CPU core; pin it away from the EO channel
  and detector.

> Pitfall: no internet on the drone AP — the page must stay self-contained (no CDN
> fonts/icons). Text labels + inline SVG only.

> Pitfall: `LIGHT` is live-fire (invisible 850 nm, eye hazard). The ON path confirms
> and the button shows a bright firing state.
