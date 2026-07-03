# Operator console — layout, controls, endpoints

The console is one self-contained web page (embedded in the binary) served by a **thin
proxy**. The app consumes the EO module's video feed and the radar daemon's frames,
forwards operator controls to each, and adds the integrated picture. **No websockets,
no framework, no CDN** (the operator laptop is on the drone link with no internet —
text labels + inline SVG + canvas only). **No capture / ISP / AE / encode / illuminator
serial in the app** — each module owns its domain.

## Layout
Full-viewport, no scroll. EO fills almost the whole screen; radar is a promotable
**PIP + swap** (limited-azimuth **sector**, not a 360° PPI). Minimal text, large touch
targets (14″ laptop → tablet). Visual language matches the Seeker ground bench: amber
`#ffb454` on near-black, cyan radar; **day** is a bright white/high-contrast theme.

- **Top bar:** `FAZE-1`, link Mb/s, BATT·ALT (reserved), ZULU (client UTC), theme, DEV.
- **EO hero:** the proxied video, amber reticle, FOV/zoom (from the EO feed), BRG/RNG
  (reserved), and the engaged-target **LOCK** (green — a projected box inside the EO
  FOV, else an edge arrow). **EO · NOT CONNECTED** scrim when the feed is down.
- **Radar scope:** rings, amber 250 m reference, cyan FOV wedge, doppler returns
  (red inbound / blue outbound / cyan static), class-less target boxes with velocity;
  engaged target as a green LOCK. **NOT CONNECTED** when the daemon is down.
- **Bottom cluster:** `LIGHT` (fire) · `ILLUM` (auto/man) · `TRACK` (auto/man) · `REC`
  (reserved); zoom ± bottom-left.
- **DEV:** stream fps/quality (forwarded to the EO feed), illuminator PWR/BEAM, radar
  display filters + cluster cfg, system temp.

## Where each thing lives (the app owns none of the sensor work)
- **EO video + zoom + AE/exposure/gain + illuminator** → the **EO module** (`:8091`).
  The console forwards those controls to its `/ctl` and shows its `/stats`.
- **Radar cloud + clustering + tracking** → the **radar daemon** (`:8092`). The console
  proxies its frames and forwards cluster cfg.
- **Console-only:** the radar scope render, EO overlays (reticle/lock), **tracking
  target-selection** (AUTO/MANUAL) + **display persistence** (hold+fade a dropped box
  ~300 ms — not coasting), day/night, layout.

## Tracking — AUTO / MANUAL (cluster `TRACK`)
- **AUTO:** engages the most important target — **fused (EO+radar) → nearer → higher
  confidence** (fused pending the detector; radar-only = nearest, tie-broken by conf).
- **MANUAL:** engages the target you **tap** (on the EO, mapped via the EO feed's FOV,
  or on the radar scope). The engaged tid flows to `/ctl`/`/stats` for the gimbal later.

## Illuminator — AUTO / MANUAL (cluster `ILLUM`)
The **EO module owns the illuminator**; the console forwards commands to its `/ctl`.
- **AUTO:** fit the beam to the camera FOV at max power — the console forwards
  `fov=<hfov>` + `power=255` when the FOV changes.
- **MANUAL:** PWR/BEAM from the DEV sliders → forwarded. `LIGHT` fires (`laser=0|1`).

## Radar tuning (DEV)
- **Display filters (client-side):** `FOV ±`, `SPEED MIN`, `RANGE MIN`. `SNR` is inert
  (firmware omits SNR).
- **Cluster cfg (→ daemon):** `CLUSTER ε` + `MIN PTS` via `/ctl` → the daemon; the
  slider reflects the **applied (clamped)** value.
- Chip cfg (SNR floor ≥17 dB, max FOV) is the radar module's job.

## HTTP endpoints (the app)
| Path | Purpose |
|---|---|
| `/` · `/app.css` · `/app.js` | the embedded page (served `no-cache`) |
| `/stream` | the EO module's JPEG frames **relayed** as MJPEG multipart, fanned to every screen |
| `/radar` | the radar daemon's latest frame **verbatim** |
| `/stats` | console state + the EO feed's `/stats` nested under `"eo"` + radar tracks |
| `/ctl?…` | routed: `track`/`engage` → local; `radar_eps`/`radar_minpts` → daemon; the rest (`zoom/laser/power/fov/ae/gain/expms/gaincap/median/fps/quality`) → the EO feed |

`/stats` top-level: `eo_connected`, `mbps`, `track` (auto/man), `engage`, `tracks`,
`radar_eps`, `radar_minpts`, `cpu_c`; reserved `null`: `batt`, `alt`, `brg`, `rng`;
plus **`eo`** — the EO feed's `/stats` object (`fps`, `sfps`, `hfov`, `vfov`, `zoom`,
`laser`, `lpower`, `lfov`, `lpresent`, `gain`, `exp_ms`, `median`, `ae`, `gaincap`) or
`null` when the feed is down.

## Notes
- **No synthetic data.** A feed that isn't up shows **NOT CONNECTED**. Never run the
  radar daemon with `-s` in front of an operator.
- **Bitrate is the EO module's.** DEV forwards plain `fps`/`quality` numbers to the EO
  feed's `/ctl` — inert until the EO module implements them (requested).
- **Format:** the EO module encodes (software MJPEG — the Orin Nano has no NVENC/NVJPG);
  the app only relays bytes. Latency = the EO feed's + one proxy hop + WiFi + decode.

> Pitfall: `LIGHT` is live-fire (invisible 850 nm, eye hazard). The ON path confirms.
