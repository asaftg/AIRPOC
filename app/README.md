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
| `main.c` | supervisor: start EO + radar + illuminator + GUI, wait for signal |
| `gui.c` / `gui.h` | shrink+compress worker (encode once) + HTTP server (`/ /stream /stats /radar /ctl`) |
| `view.c` / `view.h` | one-pass zoom-crop + box-downscale to small mono (GRAY8/Y10) |
| `eo_frame.h` | the EO→GUI "latest frame" handoff contract |
| `eo_frame_v4l2.c` | **real EO source** (default): capture+AE+ISP via the eo/pipeline module |
| `eo_frame_stub.c` | synthetic thermal EO source (`make EO_SRC=stub`, no-camera dev) |
| `radar.h` | the radar→GUI handoff contract (points + clustered target boxes) |
| `radar_stub.c` | synthetic radar source (until the AWR module lands) |
| `web/` | front-end (`index.html`, `app.css`, `app.js`) — embedded at build |
| `gen_assets.sh` | `xxd`-embeds `web/` into `web_assets.h` (single self-contained binary) |
| `systemd/` | `airpoc-app.service.in` + `install.sh` |
| `launcher/` | laptop desktop start/stop buttons (Windows `.ps1`/`.bat`, Linux `.sh`) |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev xxd
cd app && make                 # real camera (V4L2 EO) + radar + illuminator
#   make EO_SRC=stub           # deskside dev: synthetic EO, no camera needed
./app -d /dev/video0 -p 8080 -i /dev/sg-ir850     # -i illuminator optional
```
Open **`http://<jetson>:8080/`** (or `192.168.55.1:8080` over USB-C).

Install as a service (starts at boot, restarts on failure):
```bash
sh systemd/install.sh
```

## Status
- **EO** — real V4L2 provider (`eo_frame_v4l2.c`) reuses the eo/pipeline capture+AE+ISP
  and is the default build; verify on the Jetson (needs the camera). `EO_SRC=stub` is
  the no-camera dev build with a realistic thermal scene.
- **Radar** — GUI integration is done end-to-end: contract (`radar.h`), `/radar`
  endpoint, and a live polar scope (range rings, limited-azimuth FOV wedge, doppler
  returns, target boxes + trails, PIP↔hero swap). Source is `radar_stub.c` (synthetic)
  until the real AWR reader lands — a link-time swap.
- **Illuminator** — on/off·power·beam-FOV + AUTO-FOV, live via the illum shim.
- Also live: digital zoom, stream presets (fps/quality/est-Mb/s), day/night, DEV, CPU
  temp, tracks count.
- Reserved (need their modules/bus): SCAN/TRACK logic, BRG/RNG, BATT/ALT, REC, NIR
  strobe, EO detection box/confidence.

> Pitfall: the operator laptop is on the drone's WiFi with **no internet** — the page
> loads **no external fonts or icons**. Keep `web/` fully self-contained.

> Pitfall: `/dev/ttyUSB0` for the illuminator is enumeration-order; prefer the udev
> symlink `/dev/sg-ir850` (see `illuminator/docs/GUI_INTEGRATION.md`).
