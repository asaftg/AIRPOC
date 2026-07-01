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
  EO camera ─┐
  Thermal   ─┼─► detection ─┐
  Radar     ─┘              ├─► fusion ─► tracking ─► gimbal pointing ─► guidance
  NIR illuminator ──(lights the EO scene, pulsed/synced)                 (effector)
```

Sensors produce detections; fusion merges them into one target picture; tracking
maintains target state; the gimbal points the head at the track; guidance steers
an effector. The NIR illuminator is a sensing aid for the EO camera, not a
detection source.

## Compute platform

NVIDIA **Jetson Orin Nano Super** (dev-kit bring-up) today; the production seeker
targets **Xavier AGX** (it has the NVENC hardware encoder the Orin Nano lacks).
Platform bring-up — flashing, base config, cooling — is in [`jetson/`](../jetson/README.md).

## Modules

| Module | Owner | State | Chapter |
|---|---|---|---|
| Jetson platform | — | ✅ bring-up done | [`jetson/`](../jetson/README.md) |
| EO camera | — | ✅ 60 fps mono, focused, AE | [`eo/`](../eo/README.md) |
| NIR illuminator | — | 🟡 controller done, HW pending | [`illuminator/`](../illuminator/README.md) |
| Radar | — | ⬜ not started | — |
| Detection | — | ⬜ not started | — |
| Fusion | — | ⬜ not started | — |
| Tracking | — | ⬜ not started | — |
| Gimbal | — | ⬜ not started | — |
| Guidance | — | ⬜ not started | — |

### EO camera (done)
Waveshare IMX296-130 (Sony IMX296, mono global shutter) on the Jetson via a custom
`nv_imx296` driver. Streams **Y10 mono 1440×1088 @ 60 fps** with working
exposure/gain, a focus tool, a quality preview, and an MJPEG stream. Global shutter
(no rolling-shutter skew) suits fast-moving targets. Detail: [`eo/README.md`](../eo/README.md).

### NIR illuminator (in progress)
SG-IR850-8M 850 nm illuminator with motor zoom over TTL UART; C controller +
`sgctl` CLI. Purpose: light the EO scene so exposure can be short enough to freeze
a moving target. Sync to the camera exposure window is the open design item.
Detail: [`illuminator/`](../illuminator/README.md).

### Radar / Detection / Fusion / Tracking / Gimbal / Guidance (not started)
Stubs for the module owners to fill. Each should add: purpose, hardware/interfaces,
current state, and a link to its module folder.

## Production readiness

Tracked here as modules mature (what is field-ready vs prototype vs not-started).
Populate during the production-status review; keep it honest.
