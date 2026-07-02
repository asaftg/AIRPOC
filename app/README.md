# AIRPOC application (`app/`) — operator console + main process

The system's **main process** and the operator's field GUI. It starts the EO channel
and the illuminator, and serves a full-screen operator console to the operator's
laptop (and, later, a tablet) over WiFi. As radar / detection / fusion / gimbal land,
this process starts them too.

```
 EO channel (owned by eo/) ──read latest frame──►  app/ (this module)
                                                    ├─ shrink + JPEG (one small
                                                    │   picture per tick, ≤60 fps)
                                                    ├─ HTTP: / /stream /stats /ctl
                                                    └─ illuminator control (illum shim)
 operator laptop browser ◄── MJPEG + JSON over WiFi ─┘
```

Lane discipline: `app/` **reads** the EO channel's latest finished frame
(`eo_get_latest`, zero-copy) and adds no load to it. The detector consumes full-res
frames on the EO channel's own path; the GUI's shrink is a separate, display-only
branch. See [`docs/GUI.md`](docs/GUI.md) for endpoints, controls, and the compute
budget.

| File | Role |
|---|---|
| `main.c` | supervisor: start EO channel + illuminator + GUI, wait for signal |
| `gui.c` / `gui.h` | shrink+compress worker (encode once) + HTTP server (`/ /stream /stats /ctl`) |
| `view.c` / `view.h` | one-pass zoom-crop + box-downscale to small mono (GRAY8/Y10) |
| `eo_frame.h` | the EO→GUI "latest frame" handoff contract |
| `eo_frame_stub.c` | synthetic EO source (until the real channel lands) |
| `web/` | front-end (`index.html`, `app.css`, `app.js`) — embedded at build |
| `gen_assets.sh` | `xxd`-embeds `web/` into `web_assets.h` (single self-contained binary) |
| `systemd/` | `airpoc-app.service.in` + `install.sh` |
| `launcher/` | laptop desktop start/stop buttons (Windows `.ps1`/`.bat`, Linux `.sh`) |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev xxd
cd app && make
./app -d /dev/video0 -p 8080 -i /dev/sg-ir850     # -i illuminator optional
```
Open **`http://<jetson>:8080/`** (or `192.168.55.1:8080` over USB-C).

Install as a service (starts at boot, restarts on failure):
```bash
sh systemd/install.sh
```

## Status
- **EO source is the stub** (synthetic test pattern) until the EO agent exposes the
  real `eo_get_latest()` channel — then it's a link-time swap, no GUI change.
- Live: EO view + digital zoom, illuminator on/off·power·beam-FOV + AUTO-FOV, stream
  presets (fps/quality/est-Mb/s), day/night, DEV panel, CPU temp.
- Reserved (wired when the module lands): radar sector + tracks, SCAN/TRACK logic,
  BRG/RNG, BATT/ALT, REC, NIR strobe, detection box/confidence.

> Pitfall: the operator laptop is on the drone's WiFi with **no internet** — the page
> loads **no external fonts or icons**. Keep `web/` fully self-contained.

> Pitfall: `/dev/ttyUSB0` for the illuminator is enumeration-order; prefer the udev
> symlink `/dev/sg-ir850` (see `illuminator/docs/GUI_INTEGRATION.md`).
