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
  NIR illuminator ──(lights the EO scene, pulsed/synced)                 (effector)
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
| Radar | `:8092` | ✅ V2 shipped 2026-07-11: crash-proof fw + guarded temporal tracker, 26 Hz / 0 drops, class-less boxes, GUI-consumed. Human ~300 m night / ~200 m day, vehicles radial ~424 m. Open: comb gate inactive (RCA), tangential blindness (Phase 3), angle cal | [`radar/`](../radar/README.md) |
| Record & replay (`recorder/`) | `:8093` | 🟡 records the full mission (camera, radar, detections, all metadata) to the NVMe without slowing the live system, and replays it looking like the live screen — full-resolution native video (denoised, smoothly seekable H.264), radar scope + detection boxes in sync, pause/step/scrub. On-device with the real camera + radar: recording, native replay, per-session HD-convert, and offload/export all verified; browse/tag included. Next: console-side polish (native `<video>` playback + live-rate radar/det replay streams) | [`recorder/`](../recorder/README.md) |
| Detection | — | 🟡 EO detector live on `:8094` — TensorRT model (native 1440×1088) + CPU motion safety-net, one box per target, feeding the console. Stock COCO placeholder (raw-head FP16 ~20 ms / INT8 ~14.7 ms on-device); trained mono model + accuracy pending. Stateless — temporal/tracking is the EO tracker's | [`detection/`](../detection/README.md) |
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

### Detection (`detection/`) — EO object detector, running on a placeholder model
On-device detector (`detectiond`, `:8094`) that reads the EO camera tap
(`airpoc.eo_y10`) and emits per-frame `human`/`vehicle`/`drone` boxes over
`/stream` + `/stats` + `/ctl`, plus the `airpoc.det_wire` recorder tap — the same
contract shape as the radar daemon; the console already consumes it. Two paths: a
**TensorRT appearance model** at native 1440×1088 (RTMDet-tiny, Apache; raw-head
export with decode + NMS in our C++) and a **CPU motion worker** (frame-diff safety
net for movers the model missed). Where they overlap the model wins → one box per
target. Boxes carry pixels **and** real-world angle (via the lens IFOV) for fusion.

**Deliberately stateless** — one fresh list of boxes per frame; all cross-frame
work (temporal confirmation, smoothing, coasting, track IDs, detect-slow/track-fast)
is the **EO tracker's** job, which consumes this feed. Measured on-device (hot GPU,
native res): FP16 ~20.8 ms / INT8 ~14.7 ms; the model is near its floor for this
chip, and the biggest live-latency lever is pinning the GPU clocks (a `jetson/`
boot service). **Current model is a stock COCO placeholder** proving the pipeline;
the trained mono model (data/training agent) drops in with no code change. Motion
defaults **off** — the frame-diff path needs real ego-motion (IMU/VIO, or the ECC
stabilizer) before it is usable on a moving camera. Detail:
[`detection/README.md`](../detection/README.md) ·
[`detection/docs/INTEGRATION.md`](../detection/docs/INTEGRATION.md).

### Fusion / Tracking / Gimbal / Guidance (not started)
Stubs for the module owners to fill. Each should add: purpose, hardware/interfaces,
current state, and a link to its module folder. (Tracking target *selection* lives in
`app/` today; the *tracker/gimbal pointing* is the future module — and it owns the
temporal layer over the detection feed: confirm, smooth, coast, detect-slow/track-fast.)

## Production readiness

Tracked here as modules mature (what is field-ready vs prototype vs not-started).
Populate during the production-status review; keep it honest.
