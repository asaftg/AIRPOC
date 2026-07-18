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
| Operator console (`app/`) | ✅ thin **proxy** console + main process, running on the Jetson: relays EO video + radar/detector SSE, forwards controls, adds the **radar scope**, EO + detector overlays, **record/replay** (native-HD 60 fps, auto-plays cached HD, per-clip Convert-to-HD, library/offload), **tracking auto/manual**, **illuminator**, bright day/night. No capture/ISP/AE/encode. NOT CONNECTED when a feed is down. **Reserved (need their modules/bus):** gimbal pointing, BRG/RNG, BATT/ALT — see [`app/`](app/README.md) |
| Radar | ✅ **V2 shipped 2026-07-11** — 77 GHz mmWave (AWR2944P) detecting moving vehicles, drones, and people by range and Doppler. Crash-proof firmware (survives point-flood overload), temporal tracker with ghost-killing consistency guard, **26 Hz / 0 drops**, feeding the console. Measured: human ~300 m night / ~200 m day, vehicles radially to ~424 m. Firmware `agv3` (2026-07-17) fixes the comb-gate threshold scale and adds an observe mode — the junk filter now measures every detection and is awaiting its calibration before being armed. **Open:** the chip's 450-point-per-frame budget runs ~420 full, half of it spent on threshold-level junk past 200 m, which caps far-range performance; crossing traffic Doppler-blind (Phase 3); angle accuracy for EO-blind standalone guidance — see [`radar/`](radar/README.md) + [`radar/docs/ROADMAP.md`](radar/docs/ROADMAP.md) |
| Record & replay (`recorder/`) | 🟡 records the whole mission to the NVMe — full-resolution camera, radar, **detections**, and all metadata — without slowing the live system or dropping frames, and replays it looking just like the live screen: **full-resolution native video (denoised, smoothly seekable), radar scope + detection boxes in sync**, pause/step/scrub. Recordings can be browsed, tagged, **converted to a shareable HD movie**, and **offloaded to a laptop**. **On-device with the real camera + radar: EO/radar/detection recording, native replay, HD-convert, and offload all verified.** Next: console-side polish — native `<video>` playback + the live-rate radar/detection replay streams — see [`recorder/`](recorder/README.md) |
| Detection (`detection/`) | 🟡 EO object detector live on :8094 — TensorRT model at native 1440×1088 + a permissive CPU **motion safety-net** (native res, on the model's cadence; two selectable references — background-subtraction or frame-difference), merged to one box per target, feeding the console. Running a **stock COCO placeholder** (raw-head FP16 ~20 ms / INT8 ~14.7 ms on-device); trained mono model + accuracy pending (data/training agent). Motion is **off by default** until ego-motion is wired (static/holding mount only). Stateless by design — temporal confirm/track **and mover-clutter rejection** are the EO tracker's job — see [`detection/`](detection/README.md) |
| Fusion, tracking, gimbal, guidance | ⬜ not started |

A per-item production readiness review lives in the System Overview.
