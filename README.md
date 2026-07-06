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
| Radar | ✅ AWR2944P (no DCA) **C daemon + PPI previewer**, **HW-verified: 26 Hz, 0 dropped frames**, per-point SNR live; drop-free parse → range-adaptive DBSCAN → **class-less boxes** with 6 live `/ctl` knobs; **consumed by the console** (`app/radar_client.c` ← `:8092`); chip DSP timing in `/stats` (~1.9% CPU). Box/angle optimization for EO-blind **standalone guidance** = known future work — see [`radar/`](radar/README.md) |
| Record & replay (`recorder/`) | 🟡 standalone C daemon on `:8093` (systemd, deployed), **HW-verified: ~125 MB/s, 0 drops, kill-9-safe AIREC sessions, ~10% of one core**. Library + timeline replay; **native full-res replay** through the exact EO tone map (auto drift-checked vs the live feed), smooth over WiFi via cached H.264, tiered offload. **Radar taps ✅ live**; pending: EO taps ([`recorder/docs/TAP.md`](recorder/docs/TAP.md)) + console UI ([`recorder/docs/GUI_INTEGRATION.md`](recorder/docs/GUI_INTEGRATION.md)) — see [`recorder/`](recorder/README.md) |
| Detection, fusion, tracking, gimbal, guidance | ⬜ not started |

A per-item production readiness review lives in the System Overview.
