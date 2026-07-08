# detection — EO object detector (`detectiond`)

On-device detector for the AIRPOC EO camera. Consumes native IMX296 frames from
the EO pipeline's shared-memory tap and emits per-frame boxes — `human`,
`vehicle`, `drone` — for the (future) EO tracker and fusion modules. This module
**detects only**; temporal tracking and cross-sensor fusion are separate modules.

Two detection paths (see the plan): a GPU appearance model (RTMDet-tiny, native
1440×1088) and a CPU motion worker that catches any *moving* target the model
missed — far tiny drones, but equally a mid-range vehicle or a person in poor
light/shade. Where both see the same target the model box wins, so the output is
one box per target. Full design + rationale: repo plan and `docs/INTEGRATION.md`.

## Status
Standalone daemon on `:8094`: reads `airpoc.eo_y10`, serves `/stream` (SSE) /
`/stats` / `/ctl`, publishes the `airpoc.det_wire` recorder tap.
- **Appearance model** (TensorRT, `-e engine`): classified boxes (`human`,
  `vehicle`; `drone` arrives with the trained model). Stock bring-up uses a
  COCO-pretrained RTMDet-tiny.
- **Motion worker** (CPU thread, camera rate): unclassified `movers` — any moving
  target the model missed. Identity stabilizer by default (static mount); `-E`
  for ECC.
- **Model optional:** with no `-e`, the model path is a heartbeat (empty `dets`)
  while the motion path and the whole contract keep running — so the GUI and
  recorder integrate before the model lands.

## Build & run (on the Jetson)
```
cd detection && make                 # C + CUDA (nvcc) + C++ (TensorRT, OpenCV)

# build a TensorRT engine from the ONNX (FP16), on-device, once:
/usr/src/tensorrt/bin/trtexec --onnx=/data/detection/models/rtmdet_tiny.onnx \
    --fp16 --saveEngine=/data/detection/engines/rtmdet-t.fp16.trt10.engine

./detectiond -p 8094 -e /data/detection/engines/rtmdet-t.fp16.trt10.engine
./detectiond -p 8094                 # model-less: heartbeat + motion only
./detectiond -p 8094 -f 12.0         # set lens focal length (mm) for the angle mapping
curl -s localhost:8094/stats | jq
curl -N localhost:8094/stream        # live SSE detection messages
```
Flags: `-e` engine, `-s` sidecar, `-f/-x/-i` optics, `-E` ECC stabilizer, `-t` tap.
This daemon never opens `/dev/video0` — it only reads the EO pipeline's shm tap,
so run the EO pipeline (or the launcher) first.

## I/O contract (declared; full schema in `docs/INTEGRATION.md`)
- **Input:** shm tap `airpoc.eo_y10` (Y10 1440×1088, 16-slot ring) — read-only.
- **Output:** HTTP `:8094` `/stream` (SSE) + `/stats` + `/ctl`; shm tap
  `airpoc.det_wire` (byte-verbatim `/stream` JSON) for the recorder.

## Layout
```
detection/
  Makefile  README.md  docs/INTEGRATION.md  .gitignore
  tap/airpoc_tap.h            vendored tap protocol (recorder v1)
  src/ config.h coco.h        geometry + knob bounds; COCO classes + mapping
       source.h tap_source.c  frame source (live tap; replay sources land next)
       preproc.cu/.h          Y10 -> normalized model input (CUDA)
       infer.cpp/.h           TensorRT engine + RTMDet decode + NMS
       motion.cpp/.h          motion worker (OpenCV, down-res frame-diff)
       stab.h stab_identity.c stab_ecc.cpp   frame-alignment interface + impls
       http.h http.c          /stream + /stats + /ctl
       emit.h emit.c          detection-message JSON + pixel->angle mapping
       main.c                 lifecycle + path merge
```
Model + engine files live under `/data/detection/` and are never committed.
