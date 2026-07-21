# detection — EO object detector (`detectiond`)

On-device detector for the AIRPOC EO camera. Reads native IMX296 frames from the
EO pipeline's shared-memory tap and reports where people, vehicles and drones are,
frame by frame, over HTTP on `:8094` — for the EO tracker and fusion to consume.

## What it does

The camera frame goes to a neural-net model (TensorRT, native 1440×1088), which
returns boxes with a confidence score. Anything the model is confident about is
reported straight away.

The problem that shapes this module: **a confidence threshold throws away everything
below it, permanently.** A person far enough away that the model only ever half-recognises
them — scoring 0.2 on every single frame — is discarded on every single frame, and nothing
further down the chain can get them back, because the tracker only ever sees what already
survived the threshold. Raising sensitivity by simply lowering the threshold doesn't work
either: that lets in every one-frame flicker as well.

So the detector **collects weak evidence over several frames before deciding.** It runs the
model at a low floor, follows each faint candidate from frame to frame, and reports it once
it has shown up consistently enough in roughly the same place. A brief flicker fades out; a
real but faint target builds up and gets reported. Confident detections skip this entirely —
they go out immediately, unchanged, with no added delay.

Every box comes out of one place, so **a target is never reported twice**, and the reported
box is always one the model actually produced on that frame — the detector never invents or
guesses a position.

Boxes carry both pixels and a real-world angle (from the lens), because the camera has no
idea of range and angle is what fusion needs.

**How to read a box:** `tbd:1` marks one that only exists because of the frame-to-frame
collection (the model alone was not confident enough). `age`, `hits` and `disp` say how long
the evidence has been building, how many frames it was actually seen in, and how far it has
travelled in a straight line since first seen. That last one says whether the thing is
**moving or holding still** — useful to fusion, and for cross-checking against radar, which
only ever sees movers.

> **Pitfall — `disp` is not a real/false signal, and nothing downstream should treat it as
> one.** Standing still is normal for a target: parked vehicles, and people standing or
> prone, all sit at `disp ≈ 0`. Anything that dropped low-`disp` tracks would delete them.
> The distinction only holds for the *motion* worker, where every detection is a mover by
> definition — it does **not** carry over to the model.

The confidence on a box is **always the model's own score for that frame** — on a collected
box exactly as much as on a confident one. It deliberately does not reflect how much evidence
has piled up, so a collected box honestly reports a low number when that is what the model
thinks. How good the evidence is, is a different question and is answered separately by
`hits` and `age`.

> **Pitfall — do not blend the two into one percentage.** An earlier version scaled the
> collected evidence into the confidence figure so everything shared one scale. The evidence
> score saturates within a few frames, so *every* collected box came out at ~99% however
> faint the model actually found it — which made a persistent false call on a bush look more
> certain than a clearly-seen vehicle. A number that is the same for every box carries no
> information and actively misleads. Two truthful numbers beat one invented one.

The algorithm and its maths live in code comments in
[`src/temporal.h`](src/temporal.h) / [`src/temporal.c`](src/temporal.c).

## Status
- **Model: a stock, off-the-shelf COCO-trained RTMDet-tiny** (Apache-2.0, safe to ship
  closed). It is a **placeholder** that proves the pipeline. It reports `human` and
  `vehicle` (car/bus/truck); `drone` arrives with our own trained model.
- **Our trained 3-class model does NOT drop in unchanged yet.** `infer_open()` recognises
  the model's outputs by their last dimension (`== 4` → boxes, `> 4` → scores;
  `src/infer.cpp`), which a 3-class head does not match, so such an engine is rejected and
  the daemon **keeps running with no model at all** (`model=none`, no detections) while
  otherwise looking healthy. The class mapping in `src/coco.h` is also COCO-specific. Both
  need a code change before the trained model can be handed over.
- **Frame-to-frame collection: running on the Jetson.**
  On a 30 s daytime street (450 detector ticks), the plain 0.50 threshold reported 3.5
  boxes per tick and **not one person**; with collection enabled it reported 17.7 boxes per
  tick and found people. Confident boxes were identical in both runs. Most of what was added
  is real — the parked cars the threshold had been discarding.
- **Contract + recorder tap live;** the operator console already consumes `:8094`.

> **Pitfall — this is not "stateless" any more.** Earlier versions were documented as
> emitting one fresh, independent list of boxes per frame, with all cross-frame work left to
> the EO tracker. That is no longer true, and it cannot be: evidence has to be collected
> *before* the confidence threshold or it is already gone. What still belongs to the tracker
> is identity, smoothing, coasting, occlusion and re-acquisition, and long-horizon clutter
> rejection. **The tracker should read `age` / `hits` / `disp` off the wire rather than
> redo this work.**

> **Pitfall — collecting evidence makes the model's mistakes stronger too, and nothing
> downstream can undo that.** A hedge the model calls a vehicle at 0.3 on every frame is, by
> construction, indistinguishable from a real car at 0.3 on every frame. It is also
> *maximally consistent* — it appears in the same place every single frame, so it looks like
> the best-behaved target in the scene, and **no temporal test can reject it.** Persistent
> false positives are an appearance problem: only a better model removes them. On the stock
> placeholder, expect more false boxes than the trained model will give, and do not expect
> the tracker to clean them up.

> **Pitfall — tune the floor, not the frame count.** `tbd_lo` decides *how much is accepted*;
> `tbd_frames` only decides *how long a faint target waits*. Something the model keeps seeing
> gets reported eventually whatever the frame count is set to. Measured on the day clip:
> 2 → 20 frames changed the box count by 19%, while `tbd_lo` 0.15 → 0.30 halved it.

> **Pitfall — `disp` can be wrong even when the box is right.** Linking a candidate between
> frames is a simple nearest-match, and it can jump from one target to a neighbouring one.
> The box is always real; `age`/`hits`/`disp` may have picked up a neighbour's history.
> Real identity is the tracker's job.

> **Pitfall — a slewing seeker silently stops reporting faint targets. UNVALIDATED.**
> Following a candidate between frames only searches a small window around where it was:
> roughly the box size plus 24 px, at the default rate. When the whole head is moving, the
> entire image shifts by more than that and the link breaks, so faint targets never
> accumulate enough evidence and simply stop being reported — quietly, with no error.
> Confident detections are unaffected. At the current lens and detect rate:
>
> | head motion | image shift per look | link |
> |---|---|---|
> | 5°/s | 20 px | holds |
> | 10°/s | 41 px | marginal |
> | 20°/s | 81 px | **breaks** |
>
> **All validation so far is a static ground scene**, so this has never been exercised. It is
> the same weakness that made the motion worker unusable, and the real fix is the same: feed
> the head's own movement into the prediction (gimbal rate or IMU, behind `stabilize()`),
> which cannot be done until that data exists. Until then, treat faint-target reporting as a
> **search-and-hold capability, not a slew capability**, and re-check it the first time the
> detector runs on a moving mount.

### Where to set the confidence floor (measured, 30 s day clip, stock model)
| `tbd_lo` | boxes/tick | people/tick |
|---|---|---|
| 0.15 (default) | 17.7 | 4.0 |
| 0.25 | 13.4 | 2.6 |
| **0.30** | 9.3 | **0.15** |
| 0.40 | 7.9 | **0.00** |

> **Pitfall: do not raise the floor above ~0.25.** People sit right at the bottom of this
> model's confidence range, so raising the floor to cut clutter **deletes every person**
> while barely thinning the vehicles. Re-measure this table when the trained model lands —
> it is a property of the model, not of the algorithm.

> **Pitfall: swapping the model invalidates a warning in the operator console.** The console
> turns its FAINT FLOOR control red above **0.25**, and that number comes from the table
> above — i.e. from the *placeholder* model. **Whoever lands the trained model must
> re-measure the table and tell the console owner where the new threshold sits**, otherwise
> the GUI keeps warning at a level that no longer means anything. The controls themselves
> need no console change: the console hides them until `/stats` reports `knobs.temporal`, so
> they appear on their own when a build that supports them starts running.

> ### 🧊 FROZEN — the motion worker
> A separate CPU path that looked for anything *moving* the model had missed. It is **kept
> but switched off, and is not being developed.** Across four recordings it did not do its
> job: it quietly swallowed targets that move slowly or pause, missed slow distant ones, and
> on a breezy day it drowned the scene in wind-blown foliage — hundreds of boxes of real
> motion that no threshold can remove, because anything that suppresses foliage also
> suppresses distant targets.
>
> **It is frozen rather than deleted for one reason.** Collecting evidence over frames can
> rescue a target the model *half* sees, but not one it does not see **at all** — a drone a
> few pixels across that the model has no notion of. Motion is the only path that would ever
> catch that. It stays reachable (`/ctl?motion=1`) with its settings intact, so it can be
> revived if the trained model turns out to have that gap. **Do not tune it, do not put it in
> the operator GUI, and do not build on it without new evidence.**

### Speed (measured on-device, warm GPU, native resolution)
| engine | per inference | max fps |
|---|---|---|
| FP16 (**deployed**) | ~20.8 ms | ~47 |
| INT8 (built, accuracy-gated) | ~14.7 ms | ~68 |

INT8 buys about 1.5× — this model is limited by memory bandwidth, not arithmetic. At a low
detect rate the *live* figure is worse (~40 ms at 15/s) because the GPU drops its clocks
between sporadic runs; running at 30/s keeps it warm (~24 ms). **The single biggest lever is
pinning the GPU clocks**, which is a platform boot service in `jetson/`, not a detector
change. The frame-to-frame collection itself costs nothing measurable next to inference.

**Delay it adds:** only to faint targets, and only until they are confirmed — about
0.1–0.4 s at the default settings. Confident targets wait nothing at all.

### What collecting evidence costs
Switching it on does **not** change the GPU work at all — same network, same input, same
frame rate. The threshold only decides which of the network's outputs are worth looking at
afterwards, and that step runs on the CPU. Measured on the day clip:

| | per detector tick |
|---|---|
| candidates the CPU must sift | **32 → 282** (8.7×) |
| pairwise overlap checks when merging boxes | **531 → ~40,000** (75×) |
| the evidence collection itself | **4 µs** (max 14 µs) |

The collection step is free — it only ever sees boxes that survived merging (~20 a tick).
**The cost is in the box-merging stage**, which compares every surviving candidate against
every other of the same class, so 8.7× the candidates is roughly 75× that comparison work.
In context, the step before it scans all 32,130 candidate positions × 80 classes on every
tick regardless of any threshold, and that fixed scan is the larger number — so the expected
net effect is a modest rise in CPU decode time, not a 75× rise in anything end-to-end.

> **Pitfall: this has not been measured on the Jetson** — the figures above are candidate
> counts and host timings. `/stats` → `det.e2e_ms` covers the whole path including the merge,
> so **compare it with `temporal=0` and `temporal=1` on the first live run** and record the
> real number here. If it turns out to matter, `max_dets` caps the merge input directly.

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
./detectiond -p 8094            # no model: heartbeat only
curl -s localhost:8094/stats | jq
curl -N localhost:8094/stream   # live detection messages

curl -s 'localhost:8094/ctl?temporal=0'                 # collection off
curl -s 'localhost:8094/ctl?tbd_lo=0.25&tbd_frames=10'  # accept less / wait longer
```
Flags: `-e` engine, `-f/-x/-i` optics (lens focal length → the pixel-to-angle mapping),
`-E` image stabiliser (frozen motion path only), `-t` tap. Started by the launcher
(`app/launcher/start.sh`). It never opens `/dev/video0` — it only reads the EO shared-memory
tap, so run the EO pipeline (or the launcher) first.

## I/O contract (full schema in [`docs/INTEGRATION.md`](docs/INTEGRATION.md))
- **In:** shared-memory tap `airpoc.eo_y10` (Y10 1440×1088, 16-slot ring) — read-only.
- **Out:** HTTP `:8094` `/stream` (one message per detector tick) + `/stats` + `/ctl`;
  shared-memory tap `airpoc.det_wire` (the same JSON, byte for byte) for the recorder.

**Operator-facing settings — four.** `conf` (report immediately above this), `temporal`
(frame-to-frame collection on/off), `tbd_frames` (how many frames of evidence a faint target
needs) and `tbd_lo` (how faint a hint is worth collecting). Everything else — `tbd_decay`,
`tbd_max_miss`, `cadence`, `nms`, `max_dets` and all `mot_*` — is bench tuning via `curl`
and does not belong in the operator GUI.

## Layout
```
detection/
  Makefile  README.md  docs/INTEGRATION.md  .gitignore
  tap/airpoc_tap.h            vendored tap protocol (recorder v1)
  src/ config.h coco.h        geometry + settings; COCO classes + target mapping
       source.h tap_source.c  frame source (live tap; replay sources land next)
       preproc.cu/.h          Y10 -> normalized model input (CUDA)
       infer.cpp/.h           TensorRT engine + box decode + overlap merge
       temporal.c/.h          frame-to-frame evidence collection; owns the emit decision
       motion.cpp/.h          FROZEN motion worker
       stab.h stab_identity.c stab_ecc.cpp   frame-alignment interface + impls
       http.h http.c          /stream + /stats + /ctl
       emit.h emit.c          detection-message JSON + pixel-to-angle mapping
       main.c                 lifecycle + detection path + frozen-mover merge
  tools/ build_engine.cpp     ONNX -> TRT engine (FP16 / INT8)
         capture_calib.c      grab Y10 frames from the tap for INT8 calibration
         infer_probe.cpp      run a still image through the real path (model sanity check)
  tests/ test_temporal.c      unit tests for the evidence-collection decisions
```

`make test` builds and runs the unit tests. They need no GPU, no TensorRT, no OpenCV
and no board, so they run on any dev machine and can gate a change before it reaches
the Jetson. They enforce the guarantees this README and the I/O contract state —
a confident detection is passed through untouched on its first frame, one target
never yields two boxes, a faint one is reported after exactly the configured number
of frames, flicker never gets reported, no box is ever reported for a frame with no
detection, and straight-line movement grows `disp` while movement in place does not.
Models + engines live under `/data/detection/` and are never committed.

## Verify the model (bench sanity check)
`infer_probe` runs a still image through the **exact** runtime path and prints the boxes, so
the model and our decoding can be confirmed without a live target in front of the camera:
```
make tools
./infer_probe /data/detection/engines/rtmdet-t-raw.fp16.trt10.engine demo.jpg
#   car      (car           ) 0.81  px=(760,339,187,86)
```
It reads the image as mono and packs it exactly as the camera tap does. On a known image
(e.g. mmdetection's `demo.jpg`) a correct pipeline reports the obvious cars and people at
sensible confidence. This is how the current engine was verified.
