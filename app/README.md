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
   owns board + clustering + tracking                             ▼
                                              app/  (this module) ──► operator browser
                                              relay video + radar, forward controls,
                                              draw the radar scope + EO overlays +
                                              tracking selection + styling
```

Lane discipline: the app **couples to each module's served contract only**, never its
internals. When the EO agent optimized `eo/pipeline`, this app didn't break, because it
consumes the feed, not the functions. If a feed is down, the console shows **NOT
CONNECTED** — there is no synthetic data.

| File | Role |
|---|---|
| `main.c` | supervisor: start the EO + radar consumers + the GUI server, wait for a signal |
| `gui.c` / `gui.h` | proxy HTTP server: `/stream` (relay EO JPEG) · `/radar` (verbatim) · `/stats` (console + feed stats) · `/ctl` (routed) |
| `eo_client.c` / `eo_client.h` | consumes the EO module's MJPEG feed (latest JPEG + its `/stats`); forwards controls to its `/ctl` |
| `radar_client.c` / `radar.h` | consumes the radar daemon's SSE (latest frame + tracks); forwards cluster cfg to its `/ctl` |
| `web/` | front-end (`index.html`, `app.css`, `app.js`) — embedded at build |
| `gen_assets.sh` | `xxd`-embeds `web/` into `web_assets.h` (single self-contained binary) |
| `systemd/` | `airpoc-app.service.in` + `install.sh` |
| `launcher/` | always-on web START/STOP control (`:8088`) — bring the whole stack up/down from any device's browser ([README](launcher/README.md)) |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y xxd            # self-contained C — no libjpeg/eo-pipeline/illuminator link
cd app && make
./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092
```
Open **`http://<jetson>:8080/`** (or `192.168.55.1:8080` over USB-C).

The console needs the two sensor modules running:
- **EO** — `cd ../eo/pipeline && make && ./eo_pipeline` (serves the video + controls on `:8091`).
- **Radar** — `cd ../radar/src && make && ./radar_preview -w ../web` (serves SSE on `:8092`).
Either one down → the console shows that panel **NOT CONNECTED**. Never run the radar
daemon with `-s` (simulation) in front of an operator.

## Status
- **Console = pure proxy.** No capture/ISP/AE/encode/illuminator-serial on the app side.
- **EO** — proxies the EO module's MJPEG feed; forwards zoom/AE/gain/exposure/illuminator
  to its `/ctl`. On-Jetson validation of the video proxy pending. NOT CONNECTED if the
  feed is down.
- **Radar** — proxies the real daemon; renders the sector scope (rings, cyan FOV wedge,
  doppler returns, class-less target boxes with velocity, GUI display-persistence). No
  coasting in the wire (that's the tracker's job). Cluster ε / min-pts forward to the
  daemon's `/ctl`; slider reflects the applied (clamped) value.
- **Tracking** — `TRACK` AUTO/MANUAL: AUTO engages the most important target (fused →
  nearer → confidence); MANUAL engages the tapped target. Green LOCK; `engage` flows to
  `/ctl`/`/stats` for the future gimbal.
- **Illuminator** — the EO feed owns it; the console's LIGHT/PWR/BEAM + AUTO-fit forward
  to the EO feed's `/ctl`.
- Also live: **day = bright white theme**, DEV panel, CPU temp, tracks count.
- Reserved (need their modules/bus): BRG/RNG, BATT/ALT, REC, EO detection box, gimbal
  pointing.

> Pitfall: the operator laptop is on the drone's link with **no internet** — the page
> loads **no external fonts or icons**. Keep `web/` fully self-contained.

> Pitfall: single-owner V4L2 — the **EO module owns the camera** in production; the app
> never opens it. Don't run the eo/pipeline preview and another camera user at once.
