# System Overview

The system architecture for AIRPOC. This is the shared map — **each module owner
keeps their section current.** Read [Engineering Guidelines](ENGINEERING_GUIDELINES.md)
before contributing.

## Mission

Detect, track, and guide against small aerial targets (counter-UAS) and humans/vehicles (ground targets). A pan/tilt
seeker head carries the sensors; a Jetson-class compute module runs perception and
points the head at the target.

## Dataflow

```
  EO camera            ─┐
  Thermal (optional)   ─┼─► detection ─┐
  Radar                ─┘              ├─► fusion ─► tracking ─► gimbal pointing ─► guidance
  NIR illuminator ──(lights the EO scene, pulsed/synced)                 (effector)
```

Sensors produce detections; fusion merges them into one target picture; tracking
maintains target state; the gimbal points the head at the track; guidance steers
an effector. The NIR illuminator is a sensing aid for the EO camera, not a
detection source.

The **operator console (`app/`) is the system's main process**: it starts the
sensors, streams the EO video + radar scope to the operator's laptop over WiFi, and
relays operator commands back (target select for tracking, illuminator, zoom, and —
later — gimbal). It reads each sensor's latest frame read-only and never sits in the
control loop.

## Compute platform

NVIDIA **Jetson Orin Nano Super** — the compute for both bench and the production
seeker. Note it has no hardware video encoder (no NVENC), so any encoded video feed
is software MJPEG; the detector/tracker consumes frames on-device. Platform bring-up
— flashing, base config, cooling — is in [`jetson/`](../jetson/README.md).

## Modules

| Module | Owner | State | Chapter |
|---|---|---|---|
| Jetson platform | — | ✅ bring-up done | [`jetson/`](../jetson/README.md) |
| EO camera | — | ✅ 60 fps mono, AE, production C pipeline + operator monitor | [`eo/`](../eo/README.md) |
| NIR illuminator | — | ✅ controller HW-verified + controls in the reviewer; camera-sync pending | [`illuminator/`](../illuminator/README.md) |
| Operator console (`app/`) | — | 🟡 field GUI + main process: real V4L2 EO view, digital zoom, tracking auto/manual, illuminator auto/manual, radar scope, stream presets, bright day/night — on-device runtime pending Jetson power | [`app/`](../app/README.md) |
| Radar | — | 🟡 GUI scope + `radar.h` contract + synthetic source live in `app/`; real AWR reader not started | [`app/radar.h`](../app/radar.h) |
| Detection | — | ⬜ not started | — |
| Fusion | — | ⬜ not started | — |
| Tracking | — | ⬜ not started | — |
| Gimbal | — | ⬜ not started | — |
| Guidance | — | ⬜ not started | — |

### EO camera (done)
Waveshare IMX296-130 (Sony IMX296, mono global shutter) on the Jetson via a custom
`nv_imx296` driver. Streams **Y10 mono 1440×1088 @ 60 fps** with working
exposure/gain. The shipping datapath is C (`eo/pipeline/`): capture → flicker-free
AE → ISP → detector hook, plus the **operator monitor** (browser: stats overlay,
digital zoom, focus assist, illuminator controls). Python tools remain for bench
use. Global shutter (no rolling-shutter skew) suits fast-moving targets. Detail:
[`eo/README.md`](../eo/README.md).

### NIR illuminator (controls done; sync pending)
SG-IR850-8M 850 nm illuminator with motor zoom over TTL UART; C controller +
`sgctl` CLI, **HW-verified**. On/off, drive power, and beam-FOV controls are **live
in the EO operator monitor** (`eo/pipeline/`, via the illuminator shim). Purpose:
light the EO scene so exposure can be short enough to freeze a moving target. The
open item is **syncing the pulse to the camera exposure window** (see
[`NIR_SYNC.md`](../illuminator/docs/NIR_SYNC.md)). Detail: [`illuminator/`](../illuminator/README.md).

### Operator console (`app/`) — main process (in progress)
The field GUI and the system's main process. Owns the EO capture→AE→ISP→shrink→MJPEG
path (real V4L2 via `eo/pipeline`, default; a synthetic thermal source for no-camera
dev), the radar polar scope, tracking target-selection (AUTO = most important:
fused → nearer → confidence; MANUAL = tap), and illuminator control (AUTO fits the
beam to the camera FOV at max power; MANUAL from DEV). Serves the console over
**MJPEG `/stream` + polled `/stats` + `/radar` + `GET /ctl`** — no websockets, no CDN,
software MJPEG only (no NVENC/NVJPG on this SKU). Adds no load to the sensor capture
paths. Detail + endpoints: [`app/README.md`](../app/README.md) ·
[`app/docs/GUI.md`](../app/docs/GUI.md).

### Radar (GUI-integrated; sensor module not started)
The GUI already renders a live radar sector scope and consumes the point cloud +
clustered targets over the `app/radar.h` contract (`/radar`), with a synthetic source
(`app/radar_stub.c`) until the real reader lands. The **real AWR reader is not
started** — when built it implements `radar_start/stop/get_latest/set_tune`. Chip cfg
expectation: **SNR floor ≥17 dB** (below it the chip collapses) and **max FOV**, set by
the radar module (not the GUI); the GUI only filters the display and pushes DBSCAN
cluster cfg (ε / min-pts).

### Detection / Fusion / Tracking / Gimbal / Guidance (not started)
Stubs for the module owners to fill. Each should add: purpose, hardware/interfaces,
current state, and a link to its module folder. (Tracking target *selection* lives in
`app/` today; the *tracker/gimbal pointing* is the future module.)

## Production readiness

Tracked here as modules mature (what is field-ready vs prototype vs not-started).
Populate during the production-status review; keep it honest.
