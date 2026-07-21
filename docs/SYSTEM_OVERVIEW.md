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
  Radar ───────────────┴──────────────┼─► fusion ─► tracking ─► gimbal pointing ─► guidance
    └── EO-blind fallback: radar acquires → tracks → guides STANDALONE ──────────┘
  NIR illuminator ──(lights the EO scene, continuous)                    (effector)
```

Sensors produce detections; fusion merges them into one target picture; tracking
maintains target state; the gimbal points the head at the track; guidance steers
an effector. The NIR illuminator is a sensing aid for the EO camera, not a
detection source.

**Radar-only is a required capability, not just an input.** In haze, smoke, or
with the EO camera dead, radar must **acquire, track, and guide on its own** —
the chain `radar → tracking → gimbal → guidance` has to run with no EO and no
fusion. That raises the bar on the radar module: it must deliver a *steerable*
target — accurate, bias-free azimuth/elevation and a stable bounding box the
gimbal can point at — not merely a cue for EO to refine. This is why box/angle
quality (not just detection) is the radar module's open work item.

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
| Operator console (`app/`) | — | ✅ thin proxy console, running on the Jetson: relays EO video + radar/detector SSE, forwards controls, adds the radar scope + EO/detector overlays + record/replay (native-HD 60 fps, Convert-to-HD, library/offload) + tracking + day/night. No capture/ISP/AE/encode. Reserved: gimbal pointing, BRG/RNG, BATT/ALT | [`app/`](../app/README.md) |
| Radar | `:8092` | ✅ V2 shipped 2026-07-11: crash-proof fw + guarded temporal tracker, 26 Hz / 0 drops, class-less boxes, GUI-consumed. Human ~300 m night / ~200 m day, vehicles radial ~424 m. Firmware `agv3` (2026-07-17): comb gate fixed and observing, awaiting calibration before arming. Open: 450-pt/frame chip budget ~420 full (half spent on threshold-level junk past 200 m) caps far range, tangential blindness (Phase 3), angle cal | [`radar/`](../radar/README.md) |
| Record & replay (`recorder/`) | `:8093` | 🟡 records the full mission (camera, radar, detections, all metadata) to the NVMe without slowing the live system, and replays it looking like the live screen — full-resolution native video (denoised, smoothly seekable H.264), radar scope + detection boxes in sync, pause/step/scrub. On-device with the real camera + radar: recording, native replay, per-session HD-convert, and offload/export all verified; browse/tag included. Next: console-side polish (native `<video>` playback + live-rate radar/det replay streams) | [`recorder/`](../recorder/README.md) |
| Detection | — | 🟡 EO detector live on `:8094` — TensorRT model (native 1440×1088) + **track-before-detect temporal integration** (weak evidence integrated before the threshold, so far/low-contrast targets survive; strong ones unchanged and un-delayed), one box per target, feeding the console. Stock COCO placeholder (raw-head FP16 ~20 ms / INT8 ~14.7 ms on-device); trained mono model + accuracy pending. Integration validated **offline only, not yet on the Jetson**. CPU motion worker **frozen** | [`detection/`](../detection/README.md) |
| Training data (`datasets/`) | — | 🟡 offline bench pipeline (Python; never runs on the seeker): FPV-strike footage → COCO vehicle/human set for the EO detector. Architecture + non-GPU spine unit-tested on a synthetic fixture; **the real-data stages have never been run** | [`datasets/`](../datasets/README.md) |
| Fusion | — | ⬜ not started | — |
| Tracking | — | ⬜ not started | — |
| Gimbal | — | ⬜ not started | — |
| Guidance | — | ⬜ not started | — |

### EO camera (done)
Waveshare IMX296-130 (Sony IMX296, mono global shutter) on the Jetson via a custom
`nv_imx296` driver. Streams **Y10 mono 1440×1088 @ 60 fps** with working
exposure/gain. The shipping datapath is C (`eo/pipeline/`): capture → flicker-free
AE → ISP → detector hook, plus the **preview** (browser: stats overlay,
digital zoom, focus assist, illuminator controls). The earlier Python bench tools
(`eo/tools/`) were retired once the C pipeline superseded them. Global shutter
(no rolling-shutter skew) suits fast-moving targets. Detail:
[`eo/README.md`](../eo/README.md).

### NIR illuminator (controls done; continuous-on)
SG-IR850-8M 850 nm illuminator with motor zoom over TTL UART; C controller +
`sgctl` CLI, **HW-verified**. On/off, drive power, and beam-FOV controls are **live
in the EO preview** (`eo/pipeline/`, via the illuminator shim) and in the operator
console. Purpose: light the EO scene so exposure can be short enough to freeze a
moving target. The fitted device is **continuous-on — its protocol has no strobe
or trigger command** — so pulsing in step with the exposure needs a custom
illuminator and is not planned for this unit.
[`NIR_SYNC.md`](../illuminator/docs/NIR_SYNC.md) describes that future path; note
it covers a separate NIR sensor board, not this flashlight. Detail:
[`illuminator/`](../illuminator/README.md).

### Operator console (`app/`) — main process, a proxy (running on the Jetson)
The field GUI and the system's main process — a **thin proxy** that consumes the sensor
modules' feeds and adds the integrated picture. It does **no capture/ISP/AE/encode/
illuminator-serial**; each module owns its domain and the app couples to its served
contract only (so an EO/radar/detector refactor doesn't break it).

- **EO:** `app/eo_client.c` consumes the EO module's MJPEG feed (`eo/pipeline`, `:8091`),
  relays the video on `/stream`, mirrors its `/stats`, and forwards zoom/AE/gain/exposure/
  illuminator to its `/ctl`. The EO module owns the camera, ISP, AE, and the illuminator.
- **Radar:** `app/radar_client.c` consumes the radar daemon's SSE (`:8092`), serves it
  verbatim on `/radar` + `/radar/stream` per
  [`radar/docs/INTEGRATION.md`](../radar/docs/INTEGRATION.md), and forwards the ten tracker
  knobs (incl. the elevation gate).
- **Detector:** `app/det_client.c` consumes the detection daemon's SSE (`:8094`), re-broadcasts
  boxes on `/det/stream`, and forwards `det_*` knobs. Human/vehicle model boxes + motion-only
  movers are drawn over the video.
- **Record / replay:** the recorder daemon (`:8093`) is proxied at `/rec/*`. REC records; the
  LIBRARY browses/edits/deletes/offloads; replay reuses the live renderers. Native-HD 60 fps
  replay streams the recorder's cached `native.mp4` (state-driven, auto-plays when built, never
  rebuilds on entry) with a per-clip Convert-to-HD action.
- **Console-only:** the radar scope render, EO + detector overlays, tracking target-selection
  (AUTO = most important: fused → nearer → confidence; MANUAL = tap), and styling/day-night.

Serves over `/stats` + `/stream` (MJPEG) + `/radar`,`/det` (SSE) + `GET /ctl` + `/rec/*` — no
websockets, no CDN, assets sent `no-store`. A feed that is down shows **NOT CONNECTED** (no
synthetic data). Detail + endpoints:
[`app/README.md`](../app/README.md) · [`app/docs/GUI.md`](../app/docs/GUI.md).

### Radar (V2 shipped 2026-07-11; comb gate + crossing traffic open)
TI **AWR2944PEVM** (77 GHz, 4TX/4RX), **no DCA** — data is the mmw_demo TLV
point cloud over UART. The C daemon (`radar/src/`) pushes the A/G long-range
profile, parses the stream drop-free, runs a **temporal multi-target tracker**
(class-less boxes: M-of-N confirm, post-confirm consistency guard, coast,
park-hold), and serves SSE on `:8092` — consumed by the operator console
(`app/radar_client.c`). **V2** = crash-proof firmware (a point-flood frame is
deferred + counted instead of bricking the chip — field-verified), 16.0 dB
CFAR / compression 0.75 cfg, and the guard that killed the wandering-ghost
bug. **Measured:** human ~300 m night-quiet / ~200 m day-busy, vehicle radial
echoes ~424 m; 26 Hz / 0 drops (DSP-bound — see
[`radar/docs/FRAMERATE.md`](../radar/docs/FRAMERATE.md)). **Open:** the fw's
DDMA comb gate doesn't activate at runtime (RCA in progress; junk points
~250/frame until fixed); crossing traffic is Doppler-blind (Phase-3 on-chip
angle-motion detector, spec written); per-unit antenna cal for the angle
accuracy that standalone EO-blind guidance needs. Versions + plan:
[`radar/docs/ROADMAP.md`](../radar/docs/ROADMAP.md). GUI contract:
[`radar/docs/INTEGRATION.md`](../radar/docs/INTEGRATION.md). Detail:
[`radar/`](../radar/README.md).

### Record & replay (`recorder/`) — on-device; EO+radar+detection recording live, native replay working
Standalone C daemon (`:8093`, systemd `airpoc-recorder`) recording every
channel to crash-safe AIREC sessions on the NVMe (`/data/recordings`, ext4
`AIRPOC-DATA`, provisioned by `jetson/nvme/`): native Y10 (10-bit packed,
lossless), the display JPEGs the operator saw, bit-perfect radar UART bytes +
frame JSON, the EO detector frames, and 5 Hz stats/events. Producers publish to
overwrite-oldest shm taps and are never blocked; a recorder fault cannot touch a
sensor pipeline. HW-verified: 30-min full-rate soak @ ~125 MB/s with 0 drops,
kill-9 recovery to CRC-valid prefixes, recorder CPU ~10% of one core. **EO,
radar, and detection taps are all live** — real missions record camera (full-res
native + the display view), radar, detections, and metadata, and replay
end-to-end today. Replay re-serves recorded data through the same endpoint shapes
the console polls (any channel mix; a video-less session replays scope + stats),
with play/pause/0.5–4×/seek/frame-step; native full-resolution replay is a
bitrate-capped, keyframed, **grain-denoised** H.264 the console plays as a
`<video>` (seekable past 2 GB), with the radar scope and detection boxes streamed
at the recorded rate on the same timeline. Sessions can be **converted to a
shareable HD movie** on demand (persistent background encode, one at a time; a
per-session `hd` flag drives the console badge) and **offloaded** as a streaming
`.tar`. Remaining: console-side polish — native `<video>` playback and consuming
the live-rate radar/detection replay streams. Detail:
[`recorder/`](../recorder/README.md).

### Detection (`detection/`) — EO object detector with track-before-detect
On-device detector (`detectiond`, `:8094`) that reads the EO camera tap
(`airpoc.eo_y10`) and emits per-frame `human`/`vehicle`/`drone` boxes over
`/stream` + `/stats` + `/ctl`, plus the `airpoc.det_wire` recorder tap — the same
contract shape as the radar daemon; the console already consumes it. A **TensorRT
appearance model** at native 1440×1088 (RTMDet-tiny, Apache; raw-head export with
decode + NMS in our C++) feeds a **temporal integrator**. Boxes carry pixels **and**
real-world angle (via the lens IFOV) for fusion.

**Track-before-detect (v0.6.0).** A confidence threshold is a *lossy gate*: whatever
falls below it is destroyed, and no downstream tracker can recover it — so a far or
low-contrast target the model scores at 0.2 every single frame is thrown away frame
after frame. The detector therefore runs the model at a **low floor** and integrates
the weak evidence along a short trajectory before deciding what to emit. A candidate
already above `conf` goes out **immediately and unchanged** (zero added latency); a
weaker one goes out only once its accumulated evidence confirms it, flagged `"tbd":1`.
One track emits at most one box per tick, so **nothing is ever reported twice**. Boxes
carry `age`, `hits` and `disp` (net displacement) for the tracker to consume.

Measured offline on a 30 s daytime recording (real model, shipping integrator): the
0.50 gate reported **3.5 boxes/tick and zero humans**; with integration, 17.7 boxes/tick
and humans found — while the strong-tier boxes stayed byte-identical. Most of what was
added is real (the parked cars the gate was discarding). **Not yet run on the Jetson.**

> **⚠️ Contract change:** the detector is **no longer "deliberately stateless"** — it now
> carries a short-horizon temporal integrator, because evidence must be integrated *before*
> the threshold or it is gone. Identity, track IDs, coasting, occlusion, smoothing and
> long-horizon clutter rejection remain the **EO tracker's**, and the tracker must consume
> `age`/`hits`/`disp` rather than re-integrate what is already integrated here.

> **⚠️ Honest limit:** integration amplifies the model's beliefs, *including its persistent
> mistakes* — a hedge the model calls "vehicle" at 0.3 every tick is indistinguishable from
> a real car at 0.3 every tick. Only a better model fixes that, which is why the stock COCO
> placeholder is the real bottleneck. Also: `tbd_confirm` is a **latency** knob, not a
> sensitivity one (raise `tbd_lo` to accept less), and `disp` is a hint — the association
> can hop between nearby targets, so identity stays the tracker's job.

> **🧊 The CPU motion worker is FROZEN** (2026-07-21) and off by default. Measured across
> four recordings it did not do its job: it absorbs slow/near-stationary targets, misses
> slow far ones, and floods on wind-blown foliage (128–353 boxes) — clutter no per-frame
> threshold can remove. It is **frozen, not deleted**, for one reason: track-before-detect
> recovers targets the model scores *weakly* but not ones it scores at **zero** (a 3 px
> drone), and motion is the only path that would ever see those. Revive only on evidence.

Measured on-device (hot GPU, native res): FP16 ~20.8 ms / INT8 ~14.7 ms; the model is near
its floor for this chip, and the biggest live-latency lever is pinning the GPU clocks (a
`jetson/` boot service). The integrator itself costs nothing measurable. **Current model is
a stock COCO placeholder** proving the pipeline; the trained mono model (data/training
agent) does **not** yet drop in unchanged. Detail:
[`detection/README.md`](../detection/README.md) ·
[`detection/docs/INTEGRATION.md`](../detection/docs/INTEGRATION.md).

### Fusion / Tracking / Gimbal / Guidance (not started)
Stubs for the module owners to fill. Each should add: purpose, hardware/interfaces,
current state, and a link to its module folder. (Tracking target *selection* lives in
`app/` today; the *tracker/gimbal pointing* is the future module. It owns identity,
smoothing, coasting, occlusion/re-acquisition, long-horizon clutter rejection and
detect-slow/track-fast — but **not** weak-evidence integration, which now happens inside
the detector because it has to precede the confidence threshold. The tracker consumes the
detector's `age`/`hits`/`disp` instead of recomputing them.)

## Maturity

What each module has been **proven** to do — by evidence, not judgement. Three
states, each one a checkable claim:

- **Field-verified** — run on the real hardware, outdoors, against real targets,
  with a date and a recording to point at.
- **Bench-verified** — runs on the real hardware on the bench.
- **Unrun** — the code exists (and may be unit-tested) but has never been
  executed end to end on real inputs.

A row sits at the highest state its **recorded evidence** supports. Several rows
below are probably field-verified in practice and are marked bench-verified only
because no date or session id is written down anywhere. Upgrading a row means
adding that evidence — not relabelling it.

| Module | State | Evidence on record | Biggest known gap |
|---|---|---|---|
| Jetson platform | Bench-verified | JetPack 6.2.2 / L4T r36.4.4 on P3767-0005, MAXN_SUPER, fan pinned | `install_clocks.sh` / `install_fan.sh` install from `/tmp`, so the documented bring-up does not run on a fresh box; NVMe and WiFi-AP provisioning are missing from the bring-up sequence |
| EO camera | Bench-verified | Y10 mono 1440×1088 @ 60 fps on-device with working exposure/gain | the night denoiser cannot engage at defaults — its gate needs applied gain ≥ 200 while the AE cap is 120; no measured low-light image-quality figures |
| NIR illuminator | Bench-verified | on/off, power, beam FOV and status confirmed against the real device 2026-07-01 | no udev rule, so commands go to the first USB-serial adapter to enumerate; commands are fire-and-forget with no state readback |
| Operator console (`app/`) | Bench-verified | runs on the Jetson, relays EO + radar + detector + recorder, record/replay exercised on-device | the deploy procedure is written down nowhere in the repo, including the `web_assets.h` regeneration trap |
| Radar | Field-verified | V2 field-verified 2026-07-11; human ~300 m night / ~200 m day, vehicles radial ~424 m ([`radar/`](../radar/README.md)) | which firmware image is on the chip is recorded inconsistently across the radar docs; angle accuracy for EO-blind standalone guidance |
| Record & replay | Bench-verified | 30-min full-rate soak at ~125 MB/s with 0 drops; kill-9 recovery to CRC-valid prefixes | that soak has no session id, date or log to point at, and no reproduction procedure |
| Detection | Bench-verified | on-device latency FP16 ~20.8 ms / INT8 ~14.7 ms; verified correct on one known image | **no accuracy figures at all** — no mAP, no false-positive rate, no DRI ranges; and a trained 3-class model does not currently load |
| Training data (`datasets/`) | **Unrun** | the non-GPU spine unit-tested against a synthetic fixture | every stage that touches real data has never executed |
| Fusion / tracking / gimbal / guidance | Not started | — | — |
