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

The **operator console (`app/`) is the system's main process** and a **thin proxy**:
it consumes each sensor module's served feed (EO video, radar frames), forwards
operator commands to the module that owns them (zoom/AE/illuminator → EO; cluster cfg
→ radar; target-select for tracking; later, gimbal), and serves one integrated picture
to the operator's laptop over WiFi/USB/HM30. It does **no** capture/ISP/AE/encode — the
modules own their domains — and never sits in the control loop. A feed that is down
shows **NOT CONNECTED**; there is no synthetic data.

## Compute platform

NVIDIA **Jetson Orin Nano Super** — the compute for both bench and the production
seeker. Note it has no hardware video encoder (no NVENC), so any encoded video feed
is software MJPEG; the detector/tracker consumes frames on-device. Platform bring-up
— flashing, base config, cooling — is in [`jetson/`](../jetson/README.md).

## Modules

| Module | Owner | State | Chapter |
|---|---|---|---|
| Jetson platform | — | ✅ bring-up done | [`jetson/`](../jetson/README.md) |
| EO camera | — | ✅ 60 fps mono, AE, production C pipeline + preview | [`eo/`](../eo/README.md) |
| NIR illuminator | — | ✅ controller HW-verified + controls in the reviewer; camera-sync pending | [`illuminator/`](../illuminator/README.md) |
| Operator console (`app/`) | — | 🟡 thin proxy console: consumes the EO + radar feeds, forwards controls, adds the radar scope + EO overlays + tracking + day/night. No capture/ISP/AE/encode. EO video proxy pending on-Jetson validation | [`app/`](../app/README.md) |
| Radar | — | 🟨 previewer + C daemon (builds clean, sim-verified; HW bring-up pending) | [`radar/`](../radar/README.md) |
| Detection | — | ⬜ not started | — |
| Fusion | — | ⬜ not started | — |
| Tracking | — | ⬜ not started | — |
| Gimbal | — | ⬜ not started | — |
| Guidance | — | ⬜ not started | — |

### EO camera (done)
Waveshare IMX296-130 (Sony IMX296, mono global shutter) on the Jetson via a custom
`nv_imx296` driver. Streams **Y10 mono 1440×1088 @ 60 fps** with working
exposure/gain. The shipping datapath is C (`eo/pipeline/`): capture → flicker-free
AE → ISP → detector hook, plus the **preview** (browser: stats overlay,
digital zoom, focus assist, illuminator controls). Python tools remain for bench
use. Global shutter (no rolling-shutter skew) suits fast-moving targets. Detail:
[`eo/README.md`](../eo/README.md).

### NIR illuminator (controls done; sync pending)
SG-IR850-8M 850 nm illuminator with motor zoom over TTL UART; C controller +
`sgctl` CLI, **HW-verified**. On/off, drive power, and beam-FOV controls are **live
in the EO preview** (`eo/pipeline/`, via the illuminator shim) and in the operator
console. Purpose: light the EO scene so exposure can be short enough to freeze a
moving target. The open item is **syncing the pulse to the camera exposure window**
(see [`NIR_SYNC.md`](../illuminator/docs/NIR_SYNC.md)). Detail:
[`illuminator/`](../illuminator/README.md).

### Operator console (`app/`) — main process, a proxy (in progress)
The field GUI and the system's main process — a **thin proxy** that consumes the sensor
modules' feeds and adds the integrated picture. It does **no capture/ISP/AE/encode/
illuminator-serial**; each module owns its domain and the app couples to its served
contract only (so an EO/radar refactor doesn't break it).

- **EO:** `app/eo_client.c` consumes the EO module's MJPEG feed (`eo/pipeline`, `:8091`),
  relays the video on `/stream`, mirrors its `/stats`, and forwards zoom/AE/gain/exposure/
  illuminator to its `/ctl`. The EO module owns the camera, ISP, AE, and the illuminator.
- **Radar:** `app/radar_client.c` consumes the radar daemon's SSE (`:8092`), serves the
  frame verbatim on `/radar` (browser single-origin) per
  [`radar/docs/INTEGRATION.md`](../radar/docs/INTEGRATION.md), and forwards cluster cfg.
- **Console-only:** the radar scope render, EO overlays, tracking target-selection
  (AUTO = most important: fused → nearer → confidence; MANUAL = tap) with GUI display-
  persistence, and styling/day-night.

Serves over polled `/stats` + `/stream` + `/radar` + `GET /ctl` — no websockets, no CDN.
A feed that is down shows **NOT CONNECTED** (no synthetic data). Detail + endpoints:
[`app/README.md`](../app/README.md) · [`app/docs/GUI.md`](../app/docs/GUI.md).

### Radar (previewer done; on-HW bring-up pending)
TI **AWR2944PEVM** (77 GHz, 4TX/4RX), **no DCA** — data is the mmw_demo TLV
point cloud over UART. The C daemon (`radar/src/`) pushes the A/G long-range
profile, parses the stream drop-free, clusters it into **class-less** target
boxes (host DBSCAN + Kalman, until on-chip gtrack lands), and serves a PPI
previewer over SSE on `:8092`. Detects vehicles/drones/humans out to max range
(human baseline ~100 m). Builds clean and is verified end-to-end in a `-s`
simulation mode (synthetic scene through the real pipeline) so the GUI can
integrate with the board off; on-hardware bring-up pending. GUI contract:
[`radar/docs/INTEGRATION.md`](../radar/docs/INTEGRATION.md). Detail:
[`radar/`](../radar/README.md).

### Detection / Fusion / Tracking / Gimbal / Guidance (not started)
Stubs for the module owners to fill. Each should add: purpose, hardware/interfaces,
current state, and a link to its module folder. (Tracking target *selection* lives in
`app/` today; the *tracker/gimbal pointing* is the future module.)

## Production readiness

Tracked here as modules mature (what is field-ready vs prototype vs not-started).
Populate during the production-status review; keep it honest.
