# detection — EO object detector (`detectiond`)

On-device detector for the AIRPOC EO camera. Reads native IMX296 frames from the
EO pipeline's shared-memory tap and emits per-frame boxes — `human`, `vehicle`,
`drone` — for the EO tracker and fusion modules over HTTP on `:8094`.

> ### ⚠️ CONTRACT CHANGE (v0.6.0) — the detector is no longer stateless
> It used to be documented as "deliberately stateless, one fresh list of boxes per
> frame, all cross-frame work belongs to the EO tracker". **That is no longer true.**
> The detector now carries a **short-horizon temporal integrator** (track-before-detect)
> because a confidence threshold is a *lossy gate*: evidence below it is destroyed and
> no downstream tracker can ever recover it. Integrating weak evidence therefore has to
> happen **inside** the detector, before the gate.
>
> **What still belongs to the EO tracker:** identity, track IDs, coasting, occlusion and
> re-acquisition, smoothing, long-horizon clutter rejection, detect-slow/track-fast.
> **The tracker must not re-integrate what is already integrated here** — it consumes
> `age` / `hits` / `disp` off the wire instead of recomputing them.

## The two things that find targets

**1. Appearance model + temporal integration (the detection path).**
TensorRT RTMDet-tiny at native 1440×1088, run on every `cadence`-th frame. With
`temporal=1` (the default) the model is run at the low floor `tbd_lo` rather than at
`conf`, and every candidate is fed to the **track-before-detect integrator**
(`src/temporal.c`), which is the *single* place that decides what leaves the detector:

- a candidate already at/above `conf` is emitted **immediately, unchanged, with its raw
  confidence** — zero added latency, byte-identical to a non-temporal build;
- a weaker one is emitted only once its accumulated evidence crosses `tbd_confirm`,
  and is flagged **`"tbd":1`** on the wire.

Because emission is per-track through one code path, **a target can never be reported
twice** (once as "strong" and once as "integrated") in the same tick.

The score is a sequential likelihood ratio: on a hit `S += presence + (logit(conf) −
logit(tbd_lo))`, on a missed tick `S −= tbd_decay`. Evidence is *weighted*, not merely
counted — a 0.45 look confirms far faster than a 0.16 one — and flicker decays and dies
while a steady weak target climbs. The integrator never emits a predicted or coasted
box: only boxes the model actually produced on that tick, so the detector never invents
a target. Full rationale in [`src/temporal.h`](src/temporal.h).

**2. Motion worker — FROZEN.** See below. Off by default, not under development.

Full design + rationale: the repo plan and [`docs/INTEGRATION.md`](docs/INTEGRATION.md).

## Status
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
- **Temporal integration:** implemented and validated **offline only** — measured on a
  30 s daytime recording (`REC 2026-07-11 11:53 radar v2 day`, 2:58–3:28, cadence 4 ⇒
  450 ticks) by running the shipping `temporal.c` over the real model's output:

  | | boxes/tick | humans reported |
  |---|---|---|
  | today (`conf` ≥ 0.50, no integration) | 3.5 | **0** |
  | with integration (`tbd_lo` 0.15, `tbd_confirm` 3.0) | 17.7 | 4.0 |

  The strong-tier boxes were **identical** in both runs (1564 of them), confirming the
  no-latency / no-change guarantee. Most of what integration added on that clip is
  **real** — the row of parked cars the 0.50 gate was discarding — and it recovered
  people the gate reported as *nothing at all*. **Not yet run on the Jetson.**

> ### ⚠️ Honest limits of track-before-detect — read before tuning
> - **It amplifies the model's beliefs, including its persistent mistakes.** A hedge the
>   model calls "vehicle" at 0.3 *every* tick is, by construction, indistinguishable from
>   a real car at 0.3 every tick. Integration cannot fix a bad prior — only a better model
>   can. Expect more false boxes on the stock COCO placeholder than on the trained model.
> - **`tbd_confirm` is a LATENCY knob, not a sensitivity knob.** A target the model keeps
>   seeing crosses any threshold sooner or later. Measured on the clip above: sweeping
>   `tbd_confirm` 2.0 → 8.0 changed the box count by only 13%, while raising `tbd_lo`
>   0.15 → 0.25 cut it 24%. **To accept fewer things, raise `tbd_lo`.**
> - **`disp` is a hint, not a measurement.** The cross-frame link exists only to accumulate
>   evidence; it is a gated nearest-neighbour and *will* hop between nearby targets. The
>   emitted box is always the model's real box for that tick, so detections stay honest —
>   but `age`/`hits`/`disp` can belong to a hopped identity. Identity is the tracker's job.

> ### 🧊 FROZEN — the motion worker (2026-07-21)
> The motion head is **retained but not under development**, and is **off by default**.
> Measured across four recordings it does not do its job: background-subtraction *absorbs*
> slow or near-stationary targets (it lost two loitering people), frame-difference misses
> slow far targets when the baseline is short, and **both flood on wind-blown foliage**
> (128–353 surviving boxes on the day scene) — clutter no per-frame threshold can remove,
> because any threshold that kills foliage also kills far targets. Temporal integration
> supersedes it as the route to weak/far targets.
>
> **It is frozen rather than deleted for exactly one reason:** track-before-detect can
> recover a target the model scores *weakly*, but **not one the model scores at zero**
> (a 3 px drone it has no notion of). Motion is the only path that would ever see those.
> It stays reachable via `/ctl` (`motion=1`) with its knobs live, so it can be revived if
> the trained model shows that gap. **Do not tune it, do not surface it in the operator
> GUI, and do not build on it without new evidence.** Both reference methods
> (`mot_method` 0 = background-subtraction, 1 = frame-difference) are preserved as-is.

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

The integrator itself costs nothing measurable next to inference: a few hundred float
ops per candidate, no allocation after startup, fixed-size track table.

**Integration latency (weak targets only):** a promoted target waits roughly
`tbd_confirm ÷ per-hit score` ticks — with the defaults, ~2 ticks for a 0.45 candidate
and ~6 for a 0.15 one, i.e. **~0.13–0.40 s at cadence 4**. Strong targets wait zero.

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
./detectiond -p 8094            # model-less: heartbeat only
curl -s localhost:8094/stats | jq
curl -N localhost:8094/stream   # live detection messages

curl -s 'localhost:8094/ctl?temporal=0'                  # integration off
curl -s 'localhost:8094/ctl?tbd_lo=0.25&tbd_confirm=5'   # accept less / wait longer
```
Flags: `-e` engine, `-f/-x/-i` optics (lens focal → the pixel→angle mapping),
`-E` ECC stabilizer (frozen motion path only), `-t` tap. Started by the launcher
(`app/launcher/start.sh`) on the stack. Never opens `/dev/video0` — it only reads the
EO shm tap, so run the EO pipeline (or the launcher) first.

## I/O contract (full schema in [`docs/INTEGRATION.md`](docs/INTEGRATION.md))
- **Input:** shm tap `airpoc.eo_y10` (Y10 1440×1088, 16-slot ring) — read-only.
- **Output:** HTTP `:8094` `/stream` (SSE, one message per tick) + `/stats` +
  `/ctl`; shm tap `airpoc.det_wire` (byte-verbatim `/stream` JSON) for the recorder.

**Operator-facing knobs — exactly two.** `conf` and `temporal` ("EO temporal"). Every
other knob (`tbd_lo`, `tbd_confirm`, `tbd_decay`, `tbd_max_miss`, `cadence`, `nms`,
`max_dets`, and all `mot_*`) is **bench tuning reached with `curl`** and must not be
surfaced in the operator GUI.

## Layout
```
detection/
  Makefile  README.md  docs/INTEGRATION.md  .gitignore
  tap/airpoc_tap.h            vendored tap protocol (recorder v1)
  src/ config.h coco.h        geometry + knob defaults; COCO classes + target mapping
       source.h tap_source.c  frame source (live tap; replay sources land next)
       preproc.cu/.h          Y10 -> normalized model input (CUDA)
       infer.cpp/.h           TensorRT engine + raw-head decode + NMS
       temporal.c/.h          track-before-detect evidence integrator (the emit decision)
       motion.cpp/.h          FROZEN motion worker (OpenCV, native, on cadence)
       stab.h stab_identity.c stab_ecc.cpp   frame-alignment interface + impls
       http.h http.c          /stream + /stats + /ctl
       emit.h emit.c          detection-message JSON + pixel->angle mapping
       main.c                 lifecycle + detection path + frozen-mover merge
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
