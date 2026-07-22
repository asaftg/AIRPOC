# AIRPOC application (`app/`) — operator console (a proxy)

The system's **main process** and the operator's field GUI. It is a **thin console
that consumes the sensor modules' feeds** and serves one integrated operator picture
to the operator's laptop (and, later, a tablet) over WiFi/USB/HM30. It does **no
capture, no ISP, no AE, no encode, no illuminator serial** — each module owns its
domain; the app proxies and overlays.

```
 EO module (eo/pipeline, :8091)  ──MJPEG /stream + /stats + /ctl──┐
   owns camera + ISP + AE + zoom + illuminator                    │  proxy + forward
 radar module (daemon, :8092)  ──SSE /stream + /stats + /ctl──────┤  + overlays
   owns board + clustering + tracking                             │
 detection (detectiond, :8094) ──SSE /stream────────────────────┐ │
   owns the model + per-frame boxes                             │ │
                                    (input)                     ▼ │
 EO tracker (trackerd, :8095) ────SSE /stream + /stats + /ctl─────┤
   owns EO identity: stable ids, confirm/coast, engaged lock      │
 fusion (fusiond, :8096)  ───────SSE /stream + /stats + /ctl──────┤
   owns radar<->EO association: ONE target list, gids, mount trim ▼
                                              app/  (this module) ──► operator browser
                                              relay video + radar + tracks + fused targets,
                                              forward controls, draw the radar scope +
                                              EO overlays + target list + selection
```

**The EO tracker is the single source of the EO boxes** the operator sees. The detector feeds
the tracker, and its raw boxes are a DEV overlay only — drawing both was a double display of
the same targets.

Lane discipline: the app **couples to each module's served contract only**, never its
internals. When the EO agent optimized `eo/pipeline`, this app didn't break, because it
consumes the feed, not the functions. If a feed is down, the console shows **NOT
CONNECTED** — there is no synthetic data.

| File | Role |
|---|---|
| `main.c` | supervisor: start the EO + radar consumers + the GUI server, wait for a signal |
| `gui.c` / `gui.h` | proxy HTTP server: `/stream` (relay EO JPEG) · `/radar`+`/radar/stream` · `/det`+`/det/stream` · `/trk`+`/trk/stream` · `/fus`+`/fus/stream` · `/stats`/`/rstats`/`/dstats`/`/tstats`/`/fstats` · `/ctl` (routed) · `/rec/<path>` (recorder pass-through) |
| `eo_client.c` / `eo_client.h` | consumes the EO module's MJPEG feed (latest JPEG + its `/stats`); forwards controls to its `/ctl` |
| `radar_client.c` / `radar.h` | consumes the radar daemon's SSE (latest frame + tracks); re-broadcasts each frame to the browser (`/radar/stream`); forwards the ten controls to its `/ctl` |
| `trk_client.c` / `trk.h` | consumes the EO tracker's SSE (:8095) — the operator's EO boxes + engaged lock; re-broadcasts on `/trk/stream`; forwards `trk_*` knobs to its `/ctl` |
| `fus_client.c` / `fus.h` | consumes fusion's SSE (`:8096`) — the ONE target list (fused rows carry range **and** class); re-broadcasts on `/fus/stream`; forwards `fus_*` knobs to its `/ctl` |
| `det_client.c` / `det.h` | consumes the detection daemon's SSE (`:8094`, per-frame boxes + `/stats`); re-broadcasts to the browser (`/det/stream`); forwards `det_*` controls |
| `web/` | front-end (`index.html`, `app.css`, `app.js`) — embedded at build |
| `gen_assets.sh` | `xxd`-embeds `web/` into `web_assets.h` (single self-contained binary) |
| `systemd/` | `airpoc-app.service.in` + `install.sh` |
| `launcher/` | always-on web START/STOP control (`:8088`) — bring the whole stack up/down from any device's browser ([README](launcher/README.md)) |
| `docs/` | [`GUI.md`](docs/GUI.md) (UI + endpoint reference) · [`DEPLOY.md`](docs/DEPLOY.md) (push → pull → rebuild → verify, and the three traps) |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y xxd            # self-contained C — no libjpeg/eo-pipeline/illuminator link
cd app && make
./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092 -c 127.0.0.1:8093 -d 127.0.0.1:8094 -t 127.0.0.1:8095 -u 127.0.0.1:8096
```
`-e` EO feed · `-r` radar daemon · `-c` recorder daemon (`/rec/*` pass-through) · `-d` detector ·
`-t` EO tracker · `-u` fusion.
Open **`http://<jetson>:8080/`** (or `192.168.55.1:8080` over USB-C). In practice the
[launcher](launcher/README.md) (`:8088`) starts/stops the whole stack for you.

The console needs the two sensor modules running:
- **EO** — `cd ../eo/pipeline && make && ./eo_pipeline` (serves the video + controls on `:8091`).
- **Radar** — `cd ../radar/src && make && ./radar_preview -w ../web` (serves SSE on `:8092`).
Either one down → the console shows that panel **NOT CONNECTED**. Never run the radar
daemon with `-s` (simulation) in front of an operator.

## Status
- **Console = pure proxy.** No capture/ISP/AE/encode/illuminator-serial on the app side.
- **Shutdown is bounded.** Every feed client reads with a 5 s socket timeout, `*_stop()` shuts
  the live session socket down before joining, and the retry/poll sleeps run in 100 ms slices.
  Before this the console **could not be stopped at all** while its feeds were healthy: the
  reader threads sat in an unbounded `read()`, never re-read the run flag, and the join never
  returned — SIGTERM released `:8080` and left a process still consuming every feed (including
  the 60 fps EO stream). Measured on the Jetson: `SIGTERM` → **exit in 3 ms** (was: alive
  indefinitely). The same timeout also means a **wedged** daemon — socket open, nothing sent —
  ends the session and shows **NOT CONNECTED** rather than a frozen picture presented as live.
- **EO** — proxies the EO module's MJPEG feed; forwards zoom/AE/gain/exposure/illuminator
  to its `/ctl`. On-Jetson validation of the video proxy pending. NOT CONNECTED if the
  feed is down.
- **Radar** — proxies the real daemon; renders the sector scope (rings, cyan FOV wedge,
  doppler returns, class-less target boxes with velocity, GUI display-persistence). No
  coasting in the wire (that's the tracker's job). Cluster ε / min-pts forward to the
  daemon's `/ctl`; slider reflects the applied (clamped) value. Console load defaults:
  **FOV ±60°, EL ±20°**.
- **Radar → EO overlay** — radar marks drawn on the video, with **AZ/EL trim + SAVE**. The
  trim is the radar↔camera mount alignment, so it's stored **on the Jetson** (`/uiprefs` →
  `/var/lib/airpoc/ui-prefs.json`), not per browser: same value from every device and every
  address (USB / WiFi / field AP), surviving a reboot. Changes are session-only until SAVE.
- **EO tracker** — proxies `trackerd` (`:8095`, `/trk/stream`) and draws its `tracks[]` as
  **the** EO boxes: class colour, corner brackets once a track is confirmed/held, green **LOCK**
  on the engaged one, a small `t` where the detector integrated faint evidence. Tracker down →
  **EO TRACK · NOT CONNECTED**; it never falls back to raw detections.
- **Target list** — **fusion drives it when fusion is up** (`FUS` badge in the panel header),
  otherwise it falls back to the two per-sensor lists. Either way there is **no client-side
  dedup**: on the fusion wire a track that is a constituent of a fused row is never also
  published on its own, and without fusion the two sensors are simply different objects.
  `FUS` rows carry class **and** range on one line; EO rows carry class · confidence · az/el;
  radar rows carry range · speed. Rows are keyed by their **constituent** (`"eo:<eo_tid>"` /
  `"rad:<rad_tid>"`), so a tap on a row, a video box or a scope circle all land on the same
  target; the fusion `gid` picks the row colour, so it survives per-sensor id churn. The
  engaged target is pinned first.
- **Fusion** — **live on the Jetson 2026-07-22**: `fusiond` built and started by the launcher,
  the console dialled in with `-u`, both its feeds connected, and `fus_gate` set through the
  console read back applied in the daemon's own `/stats`. A **fused row has not been seen yet**
  — the scene has had no targets at all since it came up (EO `tracks[]` empty, radar 0 targets
  over 207 raw points), so the join has not had anything to join. Proxies `fusiond` (`:8096`,
  `/fus/stream`, `/fstats`, `fus_*` knobs). On the
  video, a track fusion has paired draws **heavier** (a ring around the cross in seeker mode)
  and carries the **range** fusion brought it; the radar's own circle for the other half is
  **suppressed** — one object, one mark. Fusion angles are already rig-frame (fusion owns the
  radar↔EO mount trim), so the console's display trim is never added on top of them. Fusion
  down, stale, or in replay → straight back to the per-sensor picture.
- **Tracking / selection** — tap a **list row**, an **EO box**, or a **radar scope circle**:
  all three declare the tracking state. An EO pick sends `trk_engage=<tid>` (the only thing the
  EO tracker can lock); any pick also publishes the console's own `engage=<tid>`, so a
  radar-only pick is real state rather than a dead click. `mode`/`engaged` are **reflected from
  the tracker's wire, never the button press**. **MANUAL is the default** — AUTO takes row #1 of
  the merged list, but that ranking is provisional.
- **EO detector** — proxies the detection daemon (`:8094`, `/det/stream`). Now the **tracker's
  input**, so its raw boxes are a **DEV overlay (RAW DET, off by default)** — useful to tell a
  tracker fault from a detector fault. Console load defaults: **MOTION off, MAX DETS 25,
  TEMPORAL on**. Full knob set in DEV incl. MOT MEM and the temporal controls.
- **Illuminator** — the EO feed owns it; the console's LIGHT/PWR/BEAM + AUTO-fit forward
  to the EO feed's `/ctl`.
- **Record / replay** — `REC` records via the recorder daemon (`:8093`, proxied at `/rec/*`);
  LIBRARY browses/filters/edits/deletes/offloads sessions; replay reuses the live renderers
  with a transport bar (radar + det overlays push over replay SSE). **Native HD 60fps** replay
  streams the recorder's cached `native.mp4` — state-driven, **auto-plays when ready, never
  rebuilds on entry**; each card has a **Convert to HD** button (⬆ HD → HD N% → HD ✓ only when
  actually built). Offload → `.tar` via `/rec/export` (a real download; folder via Chrome's
  "ask where to save"). **Live sensors stay up during review** — the old suspend-the-box-in-the-
  library feature was removed (it thrashed the camera and wedged the box). Save dialog takes
  name/tags (bank + custom)/note.
- **ROI + zoom** — EO/radar ROI-box zoom + EO/replay digital zoom + a radar range selector
  (AUTO/50/100/250/500 m) with constant 100/250 m reference rings — all client-side render.
- **System readouts** — Jetson junction temp, CPU % (+ core-equivalents), GPU %.
- Also live: **day = bright white theme**, DEV panel, link chip (signal/type/Mb/s/fps),
  phone-responsive layout.
- Reserved (need their modules/bus): BRG/RNG, BATT/ALT, gimbal pointing.

> Pitfall: the operator laptop is on the drone's link with **no internet** — the page
> loads **no external fonts or icons**. Keep `web/` fully self-contained.

> Pitfall: single-owner V4L2 — the **EO module owns the camera** in production; the app
> never opens it. Don't run the eo/pipeline preview and another camera user at once.
