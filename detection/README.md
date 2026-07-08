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
Phase 1 (this drop): standalone C daemon — reads `airpoc.eo_y10`, tracks feed
health, serves `/stream` (SSE) / `/stats` / `/ctl` on `:8094`, publishes the
`airpoc.det_wire` recorder tap. No GPU yet: detection messages are heartbeats
(empty `dets`/`movers`, model `none`). Later phases add the TensorRT model,
INT8, and the motion worker behind this scaffold.

## Build & run (on the Jetson)
```
cd detection && make
./detectiond -p 8094                 # consumes airpoc.eo_y10, serves :8094
./detectiond -p 8094 -f 12.0         # set lens focal length (mm) for the angle mapping
curl -s localhost:8094/stats | jq
curl -N localhost:8094/stream        # live SSE detection messages
```
Do not run while another process holds `/dev/video0`; this daemon never opens the
camera directly — it only reads the EO pipeline's shm tap, so run the EO pipeline
(or the launcher) first.

## I/O contract (declared; full schema in `docs/INTEGRATION.md`)
- **Input:** shm tap `airpoc.eo_y10` (Y10 1440×1088, 16-slot ring) — read-only.
- **Output:** HTTP `:8094` `/stream` (SSE) + `/stats` + `/ctl`; shm tap
  `airpoc.det_wire` (byte-verbatim `/stream` JSON) for the recorder.

## Layout
```
detection/
  Makefile  README.md  docs/INTEGRATION.md  .gitignore
  tap/airpoc_tap.h            vendored tap protocol (recorder v1)
  src/ config.h               geometry + knob bounds
       source.h tap_source.c  frame source (live tap; replay sources land in P2)
       http.h http.c          /stream + /stats + /ctl
       emit.h emit.c          detection-message JSON + pixel->angle mapping
       main.c                 lifecycle
```
Model + engine files live under `/data/detection/` and are never committed.
