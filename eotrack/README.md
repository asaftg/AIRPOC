# eotrack - EO tracker

The temporal layer over the EO detector. The detector (`detection/`, `:8094`) reports a
fresh list of boxes every frame with no memory. This module turns that stream into
**persistent, smoothed tracks with stable IDs**: it confirms real targets over time,
rejects wind-blown-foliage clutter, coasts through missed frames, and - when the operator
picks a target - locks onto it at full camera rate for guidance.

It produces an **EO track stream that mirrors the radar track stream** (`radar/`, `:8092`),
so the future fusion module can join the two the same way. This module does **only** the
EO side; it does not read radar, fuse, point a gimbal, or guide.

## What it consumes

| Input | Source | Used for |
|---|---|---|
| Detection boxes | detector SSE `http://127.0.0.1:8094/stream` | the measurements it tracks |
| Raw EO frames | shm tap `airpoc.eo_y10` | the 60 fps engaged-target lock, and a coarse ego-motion estimate |
| Operator selection | its own `/ctl?engage=<tid>` (forwarded by the console) | which target to lock |

A missing detector is not an error - the wire just reports `connected:false` and tracks
coast then die. A missing camera tap disables only the lock loop; stare-mode tracking is
unaffected. The module builds, launches, and is testable on its own (`make check`).

## What it produces

| Output | Where | Format |
|---|---|---|
| EO tracks | SSE `http://<host>:8095/stream` | JSON, one message per detector tick (60 Hz for the engaged track in track mode); see `docs/INTEGRATION.md` |
| Health + knobs | `http://<host>:8095/stats` | JSON |
| Control | `GET http://<host>:8095/ctl?k=v` | clamped, replies `ok` |
| Recording | shm tap `airpoc.trk_wire` | byte-verbatim of the `/stream` JSON |

### `airpoc.trk_wire` tap
16 slots, 128 KiB each, payload = the exact `/stream` JSON string. Best-effort: if the tap
can't be created the daemon logs one line and runs without it. `meta[6]` = `{frame_id,
n_tracks, 0,0,0,0}`. `t_src_ns` on the slot is the EO frame's source timestamp - a
correlation key only (**never diff it against wall-clock**; it is not on the systemwide
monotonic clock on the IMX296 driver).

## Two modes

- **stare** (default): every detector tick is associated to tracks; confirmed,
  clutter-clean tracks are emitted. This is what the operator sees on the video.
- **track** (`engage=<tid>` set): the chosen target additionally gets a 60 fps
  normalised-cross-correlation lock on the raw frames, re-anchored by each detection, so
  guidance gets camera-rate azimuth/elevation with a few ms latency. The tracker only
  reports *where the target is*; the EO, radar, and illuminator modules read the engaged
  target and each optimise their own domain to keep it. This module commands none of them.

## Design in one screen

- **Angle-domain tracks.** Internally everything is in pixels (what the detector reports
  and the lock needs); the wire carries azimuth/elevation in radians (raw sensor frame, no
  radar/EO calibration trim - that is fusion's job), plus angular size, angular rates, a
  position sigma, and a size-growth (looming) rate.
- **Association** is gated nearest-neighbour processed in ascending-distance order, with
  confirmed tracks claiming detections before tentative ones so a flood of junk can't steal
  an established target. The gate scales with the **measured** tick rate, not a configured
  one. Class is a soft penalty, not a hard gate (a placeholder model flickers class at
  range; a hard gate fragments tracks).
- **Confirmation is track hygiene, not sensitivity.** Raising weak detections above the
  noise (track-before-detect) is the **detector's** job; here a track just needs a little
  consistent evidence before it earns an ID, so one-frame junk never does.
- **Clutter rejection** is the one discrimination this module owns: over a few seconds, a
  real target nets displacement across the frame while wind-blown foliage oscillates in
  place. An oscillator is kept internally but **latched off the wire** (so what's emitted is
  always a subset of what's tracked). A target approaching head-on nets ~zero displacement
  too and is rescued by its growing size or a classified detection. There are **no
  size/speed/displacement kill gates** - those would delete exactly the far, small, slow
  targets the system exists to catch.
- **Ego-motion.** The displacement test is camera-relative, so a coarse frame-to-frame
  global-shift estimate is subtracted before the test - on a static mount it is ~0; on a
  panning gimbal it stops the whole scene reading as motion. A real IMU/VIO slots in behind
  the same shift when the gimbal exists.

## Build & run

```
make            # trackerd
make check      # offline gates: core replay + lock/ego unit tests (must pass before deploy)
./trackerd -p 8095 -d 127.0.0.1:8094 -i 287.5      # ifov urad/px (or -f focal_mm -x pixel_um)
./trackerd -p 8095 -d 127.0.0.1:8094 --no-eo       # no camera (stare only; bench)
```

On the seeker it runs under the launcher (started after the detector) and/or the systemd
unit in `systemd/`. Production build is `sm_87` (Orin Nano) like the other modules; this
module is plain C11 (no CUDA) - a small ROI NCC is sub-millisecond on one CPU core, so the
GPU stays entirely the detector's.

## Layout
```
src/      trackerd: main, core (assoc/filter/lifecycle/clutter), det_feed (SSE client),
          http, emit, eo_reader, ego, lock
tap/      vendored airpoc_tap.h (do not edit - keep byte-identical to recorder/tap/)
tools/    replay + lock_test (offline gates), fake_det (synthetic detector for bench)
docs/     INTEGRATION.md - the wire and control contract for the console and fusion
systemd/  airpoc-tracker.service + install.sh
```
