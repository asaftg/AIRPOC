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
- **Motion worker** (OpenCV, CPU thread, camera rate) — a safety net that catches
  any *moving* target the model missed (far tiny drone, or a mid-range vehicle /
  person lost to poor contrast). It models the scene with a **rolling background**
  (per-pixel median of the last few seconds), subtracts it, removes the night row
  read-noise, and confirms a mover with an M-of-N tracker over ~1 s. Emits
  unclassified `movers`. Where a mover overlaps a model box the model wins → one
  box per target.

Full design + rationale: the repo plan and [`docs/INTEGRATION.md`](docs/INTEGRATION.md).

## Status — running on the Jetson, verified
- **Model:** stock COCO-pretrained **RTMDet-tiny** (Apache-2.0), exported raw-head
  (decode done in our C++, not in the engine). Verified correct on a known image.
  This is a **placeholder** proving the pipeline; the trained mono model
  (separate data/training agent) drops in with no code change — same format, same
  endpoint. `human`/`vehicle` today (`vehicle` = car/bus/truck); `drone` comes
  with the trained model.
- **Motion:** **rolling-background** worker (median-of-recent-frames − destripe −
  M-of-N), validated offline on a night NIR recording where it holds a walking
  human that the appearance model drops. **Off by default** — the background is
  built in the current frame, so on a *moving* camera it needs ego-motion alignment
  (IMU/VIO, or the `-E` ECC stabilizer) behind `stabilize()` first. Safe on a
  static/holding mount; enable via `/ctl`. Window length is the `mot_window_s`
  knob (1–6 s, default 5).
- **Contract + recorder tap live;** the app/console already consumes `:8094`.

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
       motion.cpp/.h          motion worker (OpenCV, down-res rolling-background)
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
