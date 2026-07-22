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
- **NIR illuminator** — near-IR light so the EO camera can see and freeze
  motion in low light. Continuous-on, not pulsed.
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
eo/            EO camera module: driver, pipeline, streaming, EO docs
illuminator/   NIR illuminator module: controller + docs
radar/         radar module: AWR2944P C daemon + PPI previewer + docs
detection/     EO object detector: model + frame-to-frame evidence collection (C/CUDA/C++)
eotrack/       EO tracker: turns per-frame boxes into targets with identity + the engaged lock
recorder/      record & replay module: NVMe session recorder + library + replay
app/           the main process + operator console (field GUI) + launcher
datasets/      offline training-set builder (bench only, never runs on the seeker)
```

## Status (high level)

| Module | State |
|---|---|
| Jetson platform bring-up | ✅ flashed, MAXN, fan pinned — see [`jetson/`](jetson/README.md) |
| EO camera | ✅ Y10 mono @ 60 fps, focused, auto-exposed; **production C pipeline + preview** (zoom/focus/illuminator controls) — see [`eo/`](eo/README.md) |
| NIR illuminator | ✅ controller HW-verified **+ on/off·power·beam-FOV controls live in the EO reviewer**; camera-sync (NIR strobe) pending — see [`illuminator/`](illuminator/README.md) |
| Operator console (`app/`) | ✅ thin **proxy** console + main process, running on the Jetson: relays EO video + radar/detector/**tracker** SSE, forwards controls, adds the **radar scope**, EO overlays, **record/replay** (native-HD 60 fps, auto-plays cached HD, per-clip Convert-to-HD, library/offload), **illuminator**, bright day/night. The **EO tracker is the single source of the EO boxes** (raw detector boxes are a dev overlay), and the **target list is one list**: when the fusion daemon is up it drives the list outright (a fused row carries class **and** range together, the fused box on the video gets the range and the duplicate radar circle is suppressed), and when fusion is absent it falls back to the two source-tagged per-sensor lists. The console never merges anything itself. Tapping a list row, an EO box or a radar circle **declares the tracked target**; MANUAL is the default. No capture/ISP/AE/encode. NOT CONNECTED when a feed is down — and a feed that goes silent without closing its connection now reads as down too, instead of freezing the last picture and calling it live. **Reserved (need their modules/bus):** gimbal pointing, BRG/RNG, BATT/ALT — see [`app/`](app/README.md) |
| Radar | ✅ **V2 shipped 2026-07-11** — 77 GHz mmWave (AWR2944P) detecting moving vehicles, drones, and people by range and Doppler. Crash-proof firmware (survives point-flood overload), temporal tracker with ghost-killing consistency guard, **26 Hz / 0 drops**, feeding the console. Measured: human ~300 m night / ~200 m day, vehicles radially to ~424 m. Firmware `agv3` (2026-07-17) fixes the comb-gate threshold scale and adds an observe mode — the junk filter now measures every detection and is awaiting its calibration before being armed. **Open:** the chip's 450-point-per-frame budget runs ~420 full, half of it spent on threshold-level junk past 200 m, which caps far-range performance; crossing traffic Doppler-blind (Phase 3); angle accuracy for EO-blind standalone guidance — see [`radar/`](radar/README.md) + [`radar/docs/ROADMAP.md`](radar/docs/ROADMAP.md) |
| Record & replay (`recorder/`) | 🟡 records the whole mission to the NVMe — full-resolution camera, radar, **detections**, and all metadata — without slowing the live system or dropping frames, and replays it looking just like the live screen: **full-resolution native video (denoised, smoothly seekable), radar scope + detection boxes in sync**, pause/step/scrub. Recordings can be browsed, tagged, **converted to a shareable HD movie**, and **offloaded to a laptop**. **On-device with the real camera + radar: EO/radar/detection recording, native replay, HD-convert, and offload all verified.** Next: console-side polish — native `<video>` playback + the live-rate radar/detection replay streams — see [`recorder/`](recorder/README.md) |
| Detection (`detection/`) | 🟡 EO object detector live on :8094 — a neural-net model at native 1440×1088 that **collects faint evidence over several frames before reporting**. A confidence threshold throws away everything below it permanently, so a distant target the model only ever half-recognises was being discarded on every frame; now those faint hints are followed frame to frame and reported once they hold up (flagged `tbd`), while anything the model is already confident about goes out immediately and unchanged. One box per target, never two. On a 30 s day clip this took the detector from **3.5 boxes/tick and no people at all** to 17.7 and people found — **running on the Jetson**, where it measured as costing no extra compute. Still using a **stock, off-the-shelf placeholder model** (FP16 ~20 ms / INT8 ~14.7 ms on-device); our trained model + accuracy figures pending — and collecting evidence strengthens a weak model's mistakes too, so the model is the real bottleneck. The CPU motion worker is **frozen**, kept only because it is the one path that could ever see a target the model does not see at all — see [`detection/`](detection/README.md) |
| EO tracker (`eotrack/`) | 🟡 `trackerd` on :8095 — turns the detector's per-frame boxes into **targets with identity**: stable ids, confirm/coast through dropouts, smoothed position, and the operator-engaged lock. It is now the **single source of the EO boxes the console draws**, which removes the old double display (tracker output and raw detector boxes drawn together); the raw boxes remain as a dev overlay to tell a tracker fault from a detector fault. Console + launcher wiring **done** (`/trk/stream`, `trk_*` knobs, `trackerd` in START/STOP). Owns identity/smoothing/coasting — **not** weak-evidence integration (the detector's) and **not** cross-sensor association (fusion's). Open: end-to-end run on the box, and the crossing-vehicle ID-swap — see [`eotrack/`](eotrack/README.md) |
| Training data (`datasets/`) | 🟡 offline bench pipeline turning FPV-strike footage into a COCO vehicle/human training set for the EO detector. Architecture, schemas and the non-GPU spine are unit-tested against a synthetic fixture; **every stage that touches real data (catalog fetch, download, probe, frame extraction, GPU auto-labelling, rescan, review app) has never been run** — see [`datasets/`](datasets/README.md) |
| Fusion (`fusion/`) | 🟡 `fusiond` on :8096 — joins the radar tracker and the EO tracker into **one target picture**: a fused row carries the radar's range with the camera's angles and class under a global id, unmatched targets pass through source-tagged, and a target never appears twice. It also owns the radar↔EO mount trim. **Console + launcher wiring done and running on the Jetson** (2026-07-22: `fusiond` built and launcher-started, `/fus/stream` + `/fstats` + `fus_*` knobs live, fusion-driven target list with per-sensor fallback, fused symbology on the video). Open: **no fused row seen yet** — nothing has been in front of the rig since it came up, so the join itself is still unproven; plus the mount-trim calibration — see [`fusion/`](fusion/README.md) |
| Tracking (gimbal), guidance | ⬜ not started |

Per-module maturity — what each module is proven to do, and on what evidence — is in the [System Overview](docs/SYSTEM_OVERVIEW.md#maturity).
