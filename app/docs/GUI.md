# Operator console — layout, controls, endpoints

The console is one self-contained web page (embedded in the binary) served by a **thin
proxy**. The app consumes the EO module's video feed and the radar daemon's frames,
forwards operator controls to each, and adds the integrated picture + the record/replay
UI. **No websockets, no framework, no CDN** (the operator is on the drone link with no
internet — text labels + inline SVG + canvas + SSE only). **No capture / ISP / AE /
encode / illuminator serial in the app** — each module owns its domain.

Turning the whole stack on/off and the field networking is a separate always-on service —
see [`launcher/README.md`](../launcher/README.md) (`:8088` control page: START/STOP,
AP/WIFI/AUTO, SHUTDOWN, honest per-feed status).

## Layout
Full-viewport, no scroll. **EO fills the left ~70%**; the right column stacks a **target
list** over the **radar scope** (forward half-circle sector, not a 360° PPI). Controls
live *inside* the EO panel so they never cover the radar origin. On a **phone (≤720px)**
it stacks vertically — EO on top, radar under it, targets at the bottom; DEV + LIBRARY go
full-screen (see `app.css` media query).

- **Top bar:** `FAZE-01`, link chip (signal bars · type USB/WIFI · live Mb/s · delivered
  fps), BATT/ALT (reserved), ZULU (client UTC), NIGHT (day/night theme), **ROI**,
  LIBRARY, DEV.
- **EO (left):** proxied video, amber cross reticle, FOV/zoom + BRG/RNG lines, the
  **radar→EO overlay** (all radar targets projected onto the video — see below), detector
  boxes, zoom **±** (bottom-left), the control cluster (bottom-centre). Scrim shows
  **EO · NOT CONNECTED** / **NO VIDEO RECORDED** (replay of a radar-only session).
- **Target list (right, top):** class-less radar targets straight from the frame (the
  daemon's tracker confirms/coasts, so rows don’t flicker); tap a
  row → MANUAL-select it.
- **Radar scope (right, bottom):** see *Radar rendering* below. Expand button flips it to
  the big view (EO drops to a PIP).
- **Control cluster (over EO):** `LIGHT` (fire) · `TRACK` (auto/man) · `REC` (record).
- **DEV drawer:** a right-edge overlay (same in EO-big and radar-big) — STREAM, EO SENSOR,
  ILLUMINATOR, RADAR knobs, SYSTEM.

## Live vs replay
The same renderers show live feeds or a recording — an `API` base object swaps
`{stream,radar,stats,rstats}` to `/rec/replay/*` in replay. `body.replay` disables the
live controls (but keeps DEV ✕ / RETURN TO CONTROL usable). A striped amber banner + a
transport bar (play/pause/step/rate/scrub/hover, NATIVE↔DISPLAY when a native channel was
recorded) drive playback. Un-recorded channels show **NO VIDEO / NO RADAR RECORDED**.

## Zoom + ROI
- **EO zoom ±** — live: forwards `zoom=` to the EO feed (source crop). Replay: a
  client-side CSS zoom on the recorded frame (click to recenter).
- **ROI** (top bar) — press to arm, drag a box on the **EO or radar**, it zooms into that
  region; press again to reset. EO = CSS scale to the box; radar = a pan+zoom world window.
  Works live and in replay; resets on replay enter/leave. Purely client-side rendering.

## Radar rendering (console-owned)
- **Push, not poll.** Live radar arrives over **SSE `/radar/stream`** at the sensor's
  native rate (~27 Hz) — no polling. Replay radar comes from the recorder over the replay
  endpoint (polled). Frames are the daemon's schema, drawn verbatim.
- **FOV clip.** Only points/targets whose azimuth is within **±FOV** (`fov_half_deg`) are
  drawn — tracks the FOV knob live and the recorded value in replay.
- **Range / zoom.** `AUTO · 50 · 100 · 250 · 500 m` selector (top-left of the scope). AUTO
  = adaptive stretch (grows to fit the farthest target); the presets pin the range. Grey
  metric range-grid rings + **constant amber reference rings at 100 m and 250 m** on every
  zoom.
- Doppler colours (red inbound / blue outbound / static), per-point SNR alpha, dashed FOV
  wedge + boresight. No engaged/LOCK styling anywhere (scope, list, EO) until the tracking
  phase exists — a track wears ONE colour everywhere. **No GUI-side persistence** — the daemon is
  a temporal tracker (stable tids, M-of-N confirm, coast, park-hold), so target boxes and
  list rows are drawn verbatim from the frame; a GUI hold would double-persist.

## Radar → EO overlay (console-owned render; NOT fusion)
- Every radar target is projected onto the video from its az/el (radar frame) through the
  camera's current hfov/vfov, drawn as a **fixed-size ring + centre dot** in its track
  colour, labelled `R#tid range` (size-coding by the tracker's sx/sy was tried and pulsed —
  those estimates jitter; position is stable). No engaged/LOCK styling on the EO — all
  marks are equal until the tracking phase exists. Off-frame targets are not drawn.
- **DEV → RADAR ON EO**: OVERLAY on/off + **AZ TRIM / EL TRIM** (±10°, 0.1° steps) — the
  radar↔camera mount alignment, persisted per browser (localStorage). No rig calibration
  is stored anywhere yet (the radar module's README leaves radar↔EO calibration to the
  consumer), so defaults are 0 — nudge until a mark sits on its real object.
- Works in replay too (video + radar both come from the recording), and the trim knobs
  stay usable there — aligning against a recording is the calibration workflow.

## EO detector overlay (console-owned render; detection module owns the boxes)
- Live boxes arrive over **SSE `/det/stream`** (~15/s) from the detection daemon (`:8094`).
  `dets[]` = classified model boxes (solid; **human** cyan, **vehicle** amber, label =
  class + confidence). `movers[]` = motion-only "something moving" (dashed titanium,
  `MOT ·age` tag above the box — that tag is how you tell a motion-head box from a model
  box, which reads class+confidence instead). One box per target — the daemon dedups
  model-vs-motion overlap.
- `px` boxes are in the **native** 1440×1088 frame; the console maps them through the
  current zoom crop + the letterboxed video rect, clipped to the video content.
- **MARK style** (DEV → DETECTOR → MARK): `BOX` = full bounding boxes; `SEEKER` = a small
  gapped cross on the target centroid with short labels (`V62` / `H55` / `M·7`) —
  display-only, persisted per browser.
- Live-only for now (no recorded det channel yet); the stream closes in replay.
- Heads-up: until the trained mono model lands, the stock COCO placeholder emits false
  "vehicle" boxes on the bench — the rendering is real, today's boxes are not.

## Tracking — AUTO / MANUAL (cluster `TRACK`)
- **AUTO** engages the most important target (fused → nearer → confidence; radar-only for
  now). **MANUAL** engages the target you tap (EO, mapped via the feed's FOV, or the
  scope). `engage` flows to `/ctl`/`/stats` for the gimbal.

## Illuminator — AUTO / MANUAL (DEV → ILLUMINATOR, `LIGHT` fires)
The **EO module owns the illuminator**; the console forwards to its `/ctl`. AUTO fits the
beam to the camera FOV at max power; MANUAL uses the PWR/BEAM sliders. `LIGHT` = `laser=0|1`.

## Record / replay / library
- **REC** (cluster) starts/stops recording via the recorder daemon (`/rec/ctl?rec=…`); on
  stop, a **save dialog** (NAME / TAGS from a bank **+ a free-text custom-tag field** /
  NOTE) → `/rec/ctl?save=…&name&tags&note`, or DISCARD.
- **LIBRARY** — session cards (thumbnails or a radar-only placeholder, size badges, tags,
  the note), tag/text filters, a disk bar. **DELETE (n)** selected, **DELETE ALL** (double
  verify: confirm + type `DELETE`), **FREE — drop raw** per session (`purge_native`), and
  **OFFLOAD ALL / OFFLOAD (n)** → download a `.tar` (display video + radar + data) via
  `/rec/export`. Click a card → replay it.
- Caveat: a plain-HTTP page can't pick a save folder, so an offload `.tar` lands in the
  browser's Downloads. For a named folder use `recorder/tools/airpoc-offload.ps1 -Dest …`.

## DEV panel
- **STREAM** — QUALITY presets `PANIC / FAST / DEFAULT / NATIVE` + FPS CAP (forwarded to
  the EO feed's `/ctl` as `res`/`fps`; the detector always runs full-native).
- **EO SENSOR** — EXPOSURE auto/man, EXP ms, GAIN, AUTO-CAP, MEDIAN (→ EO feed `/ctl`).
- **ILLUMINATOR** — MODE auto/man, PWR, BEAM (→ EO feed `/ctl`).
- **RADAR ON EO** — OVERLAY on/off + AZ/EL TRIM (client-side only; see *Radar → EO overlay*).
- **DETECTOR** — CONF (min confidence to show a box), CADENCE (model runs every Nth
  frame), MOTION on/off (the dashed-mover safety net), MAX DETS, MOT SENS (`mot_k` —
  lower flags weaker motion), MOT HOLD (`mot_persist`). Forwarded namespaced
  `det_<key>=` → detection daemon `/ctl`; readback from `/dstats` (values under `knobs`).
- **RADAR** — the daemon's **nine** tracker knobs: FOV ± (default **60°**), MIN SNR,
  MIN SPD, MERGE GATE (`doppler` — velocity-coherence for the dedup merge), DEDUP (`eps` —
  merge radius for co-located boxes), MIN PTS, CONFIRM (M-of-N hits before a track
  appears, 1–6), COAST (seconds a track survives a dropout, 0–3), PARK HOLD (seconds a
  stopped mover is held, 0–60). All forward to the **daemon** namespaced `radar_<key>=`
  (the app strips the prefix → daemon `/ctl`); sliders read back the **applied (clamped)**
  value from the daemon's `/stats` (`confirm`/`coast_s`/`park_s`).
- **SYSTEM** — Jetson **TEMP** (junction/`tj` thermal zone), **CPU %** (aggregate, plus
  core-equivalents `x/N`), **GPU %** (Tegra load). RETURN TO CONTROL → the launcher (`:8088`).

## HTTP endpoints (the app)
| Path | Purpose |
|---|---|
| `/` · `/app.css` · `/app.js` | the embedded page (served `no-cache`) |
| `/stream` | EO module's JPEG frames **relayed** as MJPEG multipart, fanned to every screen |
| `/radar` | the radar daemon's latest frame **verbatim** (one-shot poll; used in replay) |
| `/radar/stream` | **SSE** push of each radar frame at native rate (`data: {…}`); the live path |
| `/det/stream` | **SSE** push of each detector message (~15/s); the live det-box path |
| `/det` | the detector's latest message verbatim (one-shot poll) |
| `/dstats` | the detector's `/stats` (health + `knobs`) for slider init |
| `/rstats` | the radar daemon's `/stats` (its control values + fps/drops) for slider init |
| `/stats` | console state + the EO feed's `/stats` nested under `"eo"` |
| `/ctl?…` | routed: `track`/`engage` → local; `radar_*` → radar daemon; `det_*` → detection daemon; the rest (`zoom/laser/power/fov/ae/gain/expms/gaincap/median/fps/res`) → the EO feed |
| `/rec/<path>` | **pass-through** to the recorder daemon (`:8093`): `/rec/ctl`, `/rec/library`, `/rec/thumbs/…`, `/rec/export`, `/rec/replay/*` |

`/stats` top-level: `eo_connected`, `mbps`, `tx_fps`, `link_type`, `rssi_dbm`, `link_mbps`,
`cpu_c` (Jetson junction °C), `cpu_pct`, `gpu_pct`, `ncpu`, `track`, `engage`, `tracks`;
reserved `null`: `batt`, `alt`, `brg`, `rng`; plus **`eo`** — the EO feed's `/stats` object
(`fps`, `sfps`, `hfov`, `vfov`, `zoom`, `laser`, `lpower`, `lfov`, `lpresent`, `gain`,
`exp_ms`, `median`, `ae`, `gaincap`, `eff_w/eff_h`, …) or `null` when the feed is down.

## Notes
- **No synthetic data.** A feed that isn't up shows **NOT CONNECTED**. Never run the radar
  daemon with `-s` in front of an operator.
- **Bitrate is the EO module's.** DEV forwards the two levers (`res`, `fps`) to the EO
  feed; on a marginal link, drop QUALITY / FPS CAP. Format is software MJPEG (the Orin Nano
  has no NVENC/NVJPG); the app only relays bytes.
- **"Recorder tap" ≠ "feed up".** A feed's port can be up (live view fine) while its
  `/dev/shm/airpoc.*` tap is gone → recordings come out empty. The launcher's `/status`
  reports this honestly (`eo_rec`/`radar_rec`); press START to re-attach.

> Pitfall: `LIGHT` is live-fire (invisible 850 nm, eye hazard). The ON path confirms.
