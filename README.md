# AIRPOC — Airborne-target Interception Proof of Concept

AIRPOC is the FAZE head one-way-attack aerial proof of concept: it
**detects, tracks, and guides against small aerial targets (A/A), humans and vehicles (A/G)**. A pan/tilt seeker
head carries the sensors; the compute stack turns their data into a track and
points the head (and, later, an effector) at the target.

**New here? Read [`docs/ENGINEERING_GUIDELINES.md`](docs/ENGINEERING_GUIDELINES.md)
first**, then [`docs/SYSTEM_OVERVIEW.md`](docs/SYSTEM_OVERVIEW.md).

## What the system does (plain English)

```
      SENSORS                 PERCEPTION                 ACT
  ┌──────────────┐      ┌────────────────────┐    ┌───────────────┐
  │ EO camera    │──┐   │ detection          │    │ gimbal        │
  │ (+ thermal)  │  ├──►│  → fusion          │──► │ pointing      │──► effector /
  │ radar        │──┘   │  → tracking        │    │ (pan/tilt)    │    drone guidance
  │ NIR illum.   │      └────────────────────┘    └───────────────┘
  └──────────────┘
```

- **EO camera** — visible mono global-shutter imaging (this repo's camera bring-up).
- **NIR illuminator** — pulsed near-IR light so the EO camera can see and freeze
  motion in low light.
- **Radar** — range/velocity detection, all-weather, day/night; the **EO-blind fallback**: must acquire/track/guide **standalone** when EO is hazed or dead (see [System Overview](docs/SYSTEM_OVERVIEW.md) dataflow).
- **Detection** — find candidate targets in each sensor stream.
- **Fusion** — combine EO + thermal + radar into one target picture.
- **Tracking** — maintain target state over time.
- **Gimbal** — point the seeker head at the track (pan/tilt).
- **Guidance** — steer an effector / interceptor toward the target.

Each is a module folder below. Modules not yet started are listed in the
[System Overview](docs/SYSTEM_OVERVIEW.md) so the map is complete.

## Repo layout

```
docs/          system-level docs (overview, engineering guidelines)
jetson/        compute-platform bring-up: flash JetPack, base config, fan
eo/            EO camera module: driver, tools, streaming, EO docs
illuminator/   NIR illuminator module: controller + docs
radar/         radar module: AWR2944P C daemon + PPI previewer + docs
detection/     EO object detector: TensorRT model + CPU motion worker (C/CUDA/C++)
recorder/      record & replay module: NVMe session recorder + library + replay
app/           the main process + operator console (field GUI)
```

## Status (high level)

| Module | State |
|---|---|
| Jetson platform bring-up | ✅ flashed, MAXN, fan pinned — see [`jetson/`](jetson/README.md) |
| EO camera | ✅ Y10 mono @ 60 fps, focused, auto-exposed; **production C pipeline + preview** (zoom/focus/illuminator controls) — see [`eo/`](eo/README.md) |
| NIR illuminator | ✅ controller HW-verified **+ on/off·power·beam-FOV controls live in the EO reviewer**; camera-sync (NIR strobe) pending — see [`illuminator/`](illuminator/README.md) |
| Operator console (`app/`) | 🟡 thin **proxy** console + main process: consumes the EO video feed + radar SSE, forwards controls, adds the **radar scope**, EO overlays, **tracking auto/manual**, **illuminator** (forwarded to the EO feed), bright day/night. No capture/ISP/AE/encode. NOT CONNECTED when a feed is down — see [`app/`](app/README.md) |
| Radar | ✅ 77 GHz mmWave radar (AWR2944P) — detects moving vehicles, drones, and people by range and Doppler, out to long range. The host clusters the point cloud (DBSCAN) into target boxes with velocity at **26 Hz, no dropped frames**; HW-verified and feeding the operator console. **Next:** tighten angle/box accuracy so radar can track and aim on its own when EO is blind (haze/night) — see [`radar/`](radar/README.md) |
| Record & replay (`recorder/`) | 🟡 records the whole mission to the NVMe — full-resolution camera, radar, **detections**, and all metadata — without slowing the live system or dropping frames, and replays it looking just like the live screen: **full-resolution native video (denoised, smoothly seekable), radar scope + detection boxes in sync**, pause/step/scrub. Recordings can be browsed, tagged, **converted to a shareable HD movie**, and **offloaded to a laptop**. **On-device with the real camera + radar: EO/radar/detection recording, native replay, HD-convert, and offload all verified.** Next: console-side polish — native `<video>` playback + the live-rate radar/detection replay streams — see [`recorder/`](recorder/README.md) |
| Detection (`detection/`) | 🟡 EO object detector live on :8094 — TensorRT model at native 1440×1088 + a CPU motion safety-net, merged to one box per target, feeding the console. Running a **stock COCO placeholder** (raw-head FP16 ~20 ms / INT8 ~14.7 ms on-device); the trained mono model + accuracy validation are pending (data/training agent). Stateless by design — temporal confirm/track is the EO tracker's job — see [`detection/`](detection/README.md) |
| Fusion, tracking, gimbal, guidance | ⬜ not started |

A per-item production readiness review lives in the System Overview.
