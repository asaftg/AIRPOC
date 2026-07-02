# AIRPOC application (`app/`) — operator console + main process

The system's **main process** and the operator's field GUI. It starts the EO channel
and the illuminator, and serves a full-screen operator console to the operator's
laptop (and, later, a tablet) over WiFi. As radar / detection / fusion / gimbal land,
this process starts them too.

```
 EO channel (owned by eo/) ──read latest frame──►  app/ (this module)
 radar module (radar.h) ────read latest frame────►  ├─ shrink + JPEG (one small
                                                    │   picture per tick, ≤60 fps)
                                                    ├─ tracking (auto/manual select)
                                                    ├─ HTTP: / /stream /stats /radar /ctl
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
| `radar.h` | radar consumer interface (subscribe to the daemon, latest-frame + stats) |
| `radar_client.c` | SSE client of the `radar/` daemon (`:8092/stream`); app serves it verbatim on `/radar` |
| `web/` | front-end (`index.html`, `app.css`, `app.js`) — embedded at build |
| `gen_assets.sh` | `xxd`-embeds `web/` into `web_assets.h` (single self-contained binary) |
| `systemd/` | `airpoc-app.service.in` + `install.sh` |
| `launcher/` | laptop desktop start/stop buttons (Windows `.ps1`/`.bat`, Linux `.sh`) |

## Build & run (on the Jetson)
```bash
sudo apt-get install -y libjpeg-turbo8-dev xxd
cd app && make                 # real camera (V4L2 EO) + radar + illuminator
#   make EO_SRC=stub           # deskside dev: synthetic EO, no camera needed
./app -d /dev/video0 -p 8080 -i /dev/sg-ir850 -r 127.0.0.1:8092   # -r radar daemon
```
Open **`http://<jetson>:8080/`** (or `192.168.55.1:8080` over USB-C).

The radar scope needs the **radar daemon** running (it publishes SSE on `:8092`):
`cd ../radar/src && make && ./radar_preview -w ../web` — or `-s` for the board-off
simulation. The app reports `radar: NO DATA` if the daemon isn't up (it never blocks).

Install as a service (starts at boot, restarts on failure):
```bash
sh systemd/install.sh
```

## Status
- **EO** — real V4L2 provider (`eo_frame_v4l2.c`) reuses the eo/pipeline capture+AE+ISP
  and is the default build; verify on the Jetson (needs the camera). `EO_SRC=stub` is
  the no-camera dev build with a realistic thermal scene.
- **Radar** — wired to the **real `radar/` daemon**: `radar_client.c` subscribes to its
  SSE `:8092/stream`, the app serves frames verbatim on `/radar` (browser stays
  single-origin), and the scope renders the daemon's point cloud + class-less target
  boxes (every target is a live this-frame detection; the GUI holds+fades a dropped box
  ~300 ms — display persistence, not coasting). Filters: FOV/SPEED/RANGE client-side; **SNR inert**
  (firmware omits SNR — Phase 2). Run the daemon in `-s` sim to develop with the board
  off. Contract: [`radar/docs/INTEGRATION.md`](../radar/docs/INTEGRATION.md).
- **Tracking** — `TRACK` is AUTO/MANUAL. AUTO engages the most important target
  (fused → nearer → higher confidence); MANUAL engages the target you tap (EO or scope).
  Engaged target renders as a green LOCK; `engage` flows to `/ctl`/`/stats`.
- **Illuminator** — `ILLUM` is AUTO/MANUAL. AUTO fits the beam to the camera FOV at max
  power (re-applied on zoom); MANUAL uses the DEV PWR/BEAM sliders. `LIGHT` fires it.
- **Radar tuning** — DEV has display filters (SNR / FOV / speed / range, client-side)
  and cluster cfg (`CLUSTER ε` / `MIN PTS` → radar module via `radar_set_tune`). Chip cfg
  (SNR ≥17 dB floor, max FOV) is the radar module's job, not the GUI.
- Also live: digital zoom, stream presets (fps/quality/est-Mb/s), **day = bright white
  theme**, DEV, CPU temp, tracks count.
- Reserved (need their modules/bus): BRG/RNG, BATT/ALT, REC, NIR strobe, EO detection
  box/confidence, gimbal pointing from the engaged track.

> Pitfall: the operator laptop is on the drone's WiFi with **no internet** — the page
> loads **no external fonts or icons**. Keep `web/` fully self-contained.

> Pitfall: `/dev/ttyUSB0` for the illuminator is enumeration-order; prefer the udev
> symlink `/dev/sg-ir850` (see `illuminator/docs/GUI_INTEGRATION.md`).
