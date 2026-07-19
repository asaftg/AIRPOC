# detection — EO object detector (`detectiond`)

On-device detector for the AIRPOC EO camera. Reads native IMX296 frames from the
EO pipeline's shared-memory tap and emits per-frame boxes — `human`, `vehicle`,
`drone` — for the EO tracker and fusion modules over HTTP on `:8094`.

**This module detects only — it is deliberately stateless (one fresh list of
boxes per frame).** All cross-frame reasoning — temporal confirmation, smoothing,
coasting, track IDs, detect-slow/track-fast — belongs to the **EO tracker**
(separate module), which consumes this feed. We emit what it needs to do that:
class + confidence, the frame id, and the exposure timestamp as the correlation key.

Two detection paths:
- **Appearance model** (TensorRT, GPU) at native 1440×1088 — classified boxes.
- **Motion worker** (OpenCV, CPU thread, on the detector `cadence`) — a **deliberately
  permissive** safety net that catches any *moving* target the model missed (far tiny
  drone, or a mid-range vehicle / person lost to poor contrast). It diffs the current
  frame against a "no-mover" reference — **two selectable methods** (`mot_method`):
  background-subtraction (median of the last `mot_window_s`) or frame-difference (vs
  `mot_baseline_s` ago) — removes the night row read-noise, and confirms a mover with an
  M-of-N tracker over ~1 s. Emits unclassified `movers`. Where a mover overlaps a model
  box the model wins → one box per target. **It is not meant to be clean:** clutter
  rejection (keep tracks that *translate*, drop wind-blown foliage that oscillates in
  place) is the EO tracker's temporal-integration job, not the detector's.

Full design + rationale: the repo plan and [`docs/INTEGRATION.md`](docs/INTEGRATION.md).

## Status — running on the Jetson, verified
- **Model:** stock COCO-pretrained **RTMDet-tiny** (Apache-2.0), exported raw-head
  (decode done in our C++, not in the engine). Verified correct on a known image.
  This is a **placeholder** proving the pipeline. `human`/`vehicle` today
  (`vehicle` = car/bus/truck); `drone` comes with the trained model.
- **A trained 3-class model does NOT yet drop in unchanged.** `infer_open()`
  identifies the output tensors by their last dimension (`== 4` -> boxes,
  `> 4` -> scores; `src/infer.cpp`), so a 3-class head (`cls [1,N,3]`) matches
  neither and the engine is rejected with `expected reg[.,4]+cls[.,C] outputs`.
  `src/main.c` then logs one line and **continues model-less** (`model=none`,
  no detections) while the daemon otherwise looks normal. The class remap in
  `src/coco.h` is COCO-index based and is itself a documented placeholder.
  Both need a code change before the trained model can be handed over.
- **Motion:** worker with **two reference methods** (`mot_method`), validated offline
  across night/day recordings — both kept so the EO-tracker phase picks the winner:
  - `0` **background-subtraction** — median of the last `mot_window_s`. Best on a stable
    scene with high-contrast movers (a bright far walker on dark road). *Absorbs* a slow /
    near-stationary target that lingers in view; costs a median rebuild.
  - `1` **frame-difference** (default) — vs the frame `mot_baseline_s` (default 2 s) ago.
    Catches the slow persistent movers the median absorbs; cheaper. A too-short baseline
    misses a slow far target; a long baseline over a moving camera needs ego-motion.

  Runs at **native resolution** (`mot_down`=1) on the **same `cadence` tick as the model**
  (one rate for both — 4 ≈ 15 Hz): far/small movers are slow in pixels, so the cadence
  rate keeps native affordable; downscaling would blind exactly the targets this net is
  for. **Off by default** — the reference is compared in the current frame, so a *moving*
  camera needs ego-motion alignment (the `-E` ECC stabilizer, IMU/VIO later) behind
  `stabilize()` first; safe on a static/holding mount. Knobs: `mot_method`,
  `mot_baseline_s`, `mot_down`, `mot_window_s`, `mot_k`, `mot_persist`, and `cadence`.
  > Pitfall: at native the worker floods on wind-blown foliage in daytime — genuine
  > motion, not noise. It's rejected **downstream by the EO tracker** (foliage oscillates
  > in place; a real target translates), not by any detector threshold — no `mot_k` /
  > baseline setting removes foliage without also deleting the far targets.
- **Contract + recorder tap live;** the app/console already consumes `:8094`.

> ### ⚠️ OPEN DECISION — two motion methods ship on purpose (revisit in the tracker phase)
> The motion worker deliberately carries **both** reference methods behind the `mot_method`
> knob — `0` background-subtraction and `1` frame-difference — because on our recordings
> **neither is a clear winner**: bg-sub *absorbs* slow/near-stationary movers (it lost two
> loitering people), frame-diff catches those but needs the right `mot_baseline_s`.
> **This is temporary.** The **EO-tracker phase must A/B the two on real tracked targets,
> pick the winner, and delete the loser** — do not assume one is chosen. Evidence lives in
> the session notes; the default today is frame-diff (`mot_method=1`).

### Latency (measured on-device, hot GPU, native resolution)
| engine | per-inference | max fps |
|---|---|---|
| FP16 raw-head (**deployed default**) | ~20.8 ms | ~47 |
| INT8 raw-head (built, accuracy-gated) | ~14.7 ms | ~68 |

Notes: INT8 buys ~1.5× (this model is memory-bound, not compute-bound). At a
*low* detect rate the live latency is higher (~40 ms at 15 fps) because the GPU
downclocks between sporadic runs — running at 30 fps keeps it warm (~24 ms/frame,
~64% GPU). The single biggest live-latency lever is **pinning the GPU clocks**, a
platform boot service in `jetson/` — not a detector change. INT8 is held as an
option for the terminal high-frame-rate phase, pending an accuracy gate on the
trained model.

## Build & run (on the Jetson)
```
cd detection && make            # C + CUDA (nvcc) + C++ (TensorRT, OpenCV)
make tools                      # build_engine + capture_calib

# 1) get an ONNX under /data/detection/models/ (raw-head export; see docs)
# 2) build the engine on-device (per-device / per-TRT-version, gitignored):
./build_engine --onnx /data/detection/models/rtmdet_tiny_rawhead.onnx --fp16 \
    --out /data/detection/engines/rtmdet-t-raw.fp16.trt10.engine
#    INT8 (calibrated on captured frames): ./capture_calib 200 /data/detection/calib
#    then add:  --int8 --calib /data/detection/calib

./detectiond -p 8094 -e /data/detection/engines/rtmdet-t-raw.fp16.trt10.engine
./detectiond -p 8094            # model-less: motion + heartbeat only
curl -s localhost:8094/stats | jq
curl -N localhost:8094/stream   # live detection messages
```
Flags: `-e` engine, `-f/-x/-i` optics (lens focal → the pixel→angle mapping),
`-E` ECC stabilizer, `-t` tap. Started by the launcher (`app/launcher/start.sh`)
on the stack. Never opens `/dev/video0` — it only reads the EO shm tap, so run the
EO pipeline (or the launcher) first.

## I/O contract (full schema in [`docs/INTEGRATION.md`](docs/INTEGRATION.md))
- **Input:** shm tap `airpoc.eo_y10` (Y10 1440×1088, 16-slot ring) — read-only.
- **Output:** HTTP `:8094` `/stream` (SSE, one message per tick) + `/stats` +
  `/ctl` (live knobs: `conf`, `cadence`, `motion`, …); shm tap `airpoc.det_wire`
  (byte-verbatim `/stream` JSON) for the recorder.

## Layout
```
detection/
  Makefile  README.md  docs/INTEGRATION.md  .gitignore
  tap/airpoc_tap.h            vendored tap protocol (recorder v1)
  src/ config.h coco.h        geometry + knob defaults; COCO classes + target mapping
       source.h tap_source.c  frame source (live tap; replay sources land next)
       preproc.cu/.h          Y10 -> normalized model input (CUDA)
       infer.cpp/.h           TensorRT engine + raw-head decode + NMS
       motion.cpp/.h          motion worker (OpenCV, native, bg-sub | frame-diff, on cadence)
       stab.h stab_identity.c stab_ecc.cpp   frame-alignment interface + impls
       http.h http.c          /stream + /stats + /ctl
       emit.h emit.c          detection-message JSON + pixel->angle mapping
       main.c                 lifecycle + two-path merge
  tools/ build_engine.cpp     ONNX -> TRT engine (FP16 / INT8 w/ entropy calibrator)
         capture_calib.c      grab Y10 frames from the tap for INT8 calibration
         infer_probe.cpp      run a still image through the real infer path (model sanity check)
```
Models + engines live under `/data/detection/` and are never committed.

## Verify the model (bench sanity check)
`infer_probe` runs a still image through the **exact** runtime path (preproc.cu +
the engine + raw-head decode + NMS) and prints the boxes — so you can confirm the
model and our decode are correct without a live target in front of the camera:
```
make tools
./infer_probe /data/detection/engines/rtmdet-t-raw.fp16.trt10.engine demo.jpg
#   car      (car           ) 0.81  px=(760,339,187,86)
#   ...
```
It reads the image as mono and stretches it to the model input, then packs Y10
exactly as the tap does. Use a known image (e.g. mmdetection's `demo.jpg`): a
correct pipeline reports the obvious cars/people at sensible confidence. This is
how the current stock engine was verified.
```
