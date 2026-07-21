# detection — I/O contract

`detectiond` is a standalone process. It reads the EO camera tap and serves the
same `/stream` + `/stats` + `/ctl` shape as the radar daemon, plus a recorder
tap. Port **8094**.

## Input — `airpoc.eo_y10` (read-only)
EO pipeline shm tap, protocol v1 (`tap/airpoc_tap.h`). 16 slots, payload
1440×1088×2 = 3,133,440 B. Pixels are Y10 in 16-bit LE words: value =
`(b0 | b1<<8) >> 6`, range 0..1023, stride 2880. Per-slot metadata used here:

| field | meaning |
|---|---|
| `t_src_ns` | V4L2/driver exposure timestamp — use as a **frame-correlation key** (same value the recorder stores); on the IMX296 driver it is NOT on the CLOCK_MONOTONIC timebase (observed ~30 s offset), so do not diff it against wall-clock times |
| `t_pub_ns` | when the EO pipeline published the frame (CLOCK_MONOTONIC, systemwide) |
| `meta[0]` | v4l2 sequence (frame id; gaps = driver drops) |
| `meta[4]` | illuminator state: `on \| present<<1 \| power<<8 \| fov10<<16` |
| `meta[5]` | cumulative driver drops |

The daemon self-heals across EO-pipeline restarts (stale mapping → reopen).

## Output — `GET /stream` (Server-Sent Events)
One `data: <json>\n\n` per detector tick (every cadence-th captured frame),
emitted **even when empty** so it doubles as a heartbeat. Schema:

```json
{"type":"det","frame_id":184223,
 "t_src_ns":123456789012,"t_pub_ns":123456790012,"t_out_ns":123456805000,
 "latency_ms":16.0,
 "night":true,"illum":{"on":true,"present":true,"power":40,"fov10":85},
 "model":"rtmdet-t_v3.1ch.int8","img":{"w":1440,"h":1088},"ifov_urad":287.5,
 "tap_gaps":0,"drops_cum":0,
 "dets":[
   {"src":"app","cls":"drone","age":11,"hits":9,"disp":34.2,"conf":0.83,
    "px":[912.4,301.2,14.0,9.5],"ang":[0.0553,0.0698,0.0040,0.0027]},
   {"src":"app","cls":"human","age":6,"hits":6,"disp":12.8,"tbd":1,"conf":0.57,
    "px":[640.0,712.0,9.0,21.0],"ang":[-0.0229,-0.0484,0.0026,0.0060]}],
 "movers":[]}
```

- `px` = `[cx, cy, w, h]` in full-resolution pixels.
- `ang` = `[az, el, w, h]` in **radians**: `az = (cx - 719.5)·ifov`,
  `el = (543.5 - cy)·ifov` (az +right, el +up), `ifov` from the lens
  (default 287.5 µrad/px at f=12 mm — set with `-f`/`-i`). Angle is what fusion
  consumes; the camera has no range.
- `src`: `"app"` = appearance model, `"mot"` = motion worker (**frozen**, off by
  default — `movers` is normally empty). Where the two overlap the model box wins
  and the mover is dropped → **one box per target**.
- `cls`: `human` / `vehicle` / `drone` for model boxes; omitted for unclassified
  movers.
- `t_src_ns` is the frame-correlation key (see input table — driver clock, not
  CLOCK_MONOTONIC). `latency_ms = (t_out_ns - t_pub_ns)/1e6` is the detector
  pipeline time (EO-publish → we emit); `t_pub_ns`/`t_out_ns` are both
  CLOCK_MONOTONIC so the diff is valid.
- `cls` values from the stock COCO model are `human` (person) and `vehicle`
  (car/bus/truck only — not bicycle/motorcycle, which aren't target vehicles);
  `drone` arrives with the trained model. With no engine loaded, `model` is
  `"none"` and `dets` is empty.

### Evidence-collection fields (model boxes, when `temporal=1`)
Additive and optional — a consumer that ignores them behaves exactly as before.

| field | meaning |
|---|---|
| `age` | ticks since this evidence was first seen |
| `hits` | frames it was actually seen in (`hits < age` ⇒ it was missed on some ticks) |
| `disp` | how far it has moved in a **straight line**, in pixels, since first seen. Something crossing the scene grows this steadily; something jiggling in place (a wind-blown branch) keeps it near zero however long it lives. **A hint, not a measurement** — see the caveat below. |
| `tbd` | present and `1` **only** when the box exists because of collected evidence, i.e. the model alone scored it *below* `conf`. Absent means it cleared `conf` on its own. |

**Guarantee: one box per target per tick.** Confident and collected detections leave through
a single path, so a target is never reported twice. Boxes are always what the model actually
produced on that tick — the detector never reports a guessed or coasted position.

**Delay:** a `"tbd":1` box waited `age` ticks while its evidence built up (~0.1–0.4 s at the
default settings). A box **without** `tbd` waited nothing — it is identical to what a build
with collection switched off would have reported on that tick.

**Caveat on `age`/`hits`/`disp`:** linking a candidate from one frame to the next is a simple
nearest-match, there only to decide whether evidence is still building for the same thing. It
*can* jump to a neighbouring target. The box stays correct; those three counters may have
picked up a neighbour's history. **Real identity is the EO tracker's job** — and the tracker
should read these fields rather than recompute them.

## Output — `GET /stats`
```json
{"version":"0.6.0","ifov_urad":287.5,"img":{"w":1440,"h":1088},
 "tap":{"connected":true,"fps":60.0,"gaps":0,"drops_cum":0,"frame_id":184223},
 "det":{"active":false,"fps":0.0,"model":"none","precision":"",
        "infer_ms":{"p50":0,"p95":0},"e2e_ms":{"p50":0,"p95":0}},
 "temporal":{"active":true,"tracks":14,"promoted":6},
 "motion":{"active":false,"frozen":true,"fps":0.0,"stab_fail_pct":0.0,"candidates":0},
 "knobs":{"conf":0.50,"cadence":4,"max_dets":128,"nms":0.45,
          "temporal":1,"tbd_frames":6,"tbd_lo":0.15,"tbd_decay":0.70,
          "tbd_max_miss":3,
          "motion":0,"mot_k":6.0,"mot_window_s":15.0,"mot_persist":3,"mot_down":1,
          "mot_method":1,"mot_baseline_s":2.0}}
```
`det.active` is true once an engine is loaded (`-e`). `temporal.tracks` is how many faint candidates are
currently being followed (including ones not yet reported, so it is normally larger than the
box count); `temporal.promoted` is how many boxes on the last tick were `"tbd":1`. `motion.active` is true once the frozen motion thread has
processed a frame; `motion.frozen` is always `true` (see the module README).

## Output — `GET /ctl?k=v&...`
Sets live knobs; absent params keep their value; all clamped server-side;
replies `ok`.

**Four settings are operator-facing** and belong in the GUI: `conf`, `temporal`,
`tbd_frames` and `tbd_lo`. Everything else is bench tuning reached with `curl` and must
not be surfaced in the operator GUI.

> Note for consumers: `/ctl` replies `ok` (200) to **any** request — unknown parameters are
> silently ignored. A 200 therefore proves only that the request arrived, never that this
> build honours the setting. To test whether a knob exists, look for it in `/stats` under
> `knobs.*` (the operator console gates its controls on `knobs.temporal` for this reason).

| knob | range | meaning |
|---|---|---|
| `conf` | 0.05–0.95 | **report immediately above this**: a detection at or above this confidence is reported on its own, unchanged, with no added delay |
| `temporal` | 0/1 | **frame-to-frame evidence collection on/off** (the "EO temporal" button). On: the model is run at `tbd_lo` and faint candidates are followed across frames before anything is reported. Off: the model runs at `conf` and its boxes are reported directly. |
| `tbd_frames` | 2–20 | **how many frames of evidence** a target sitting exactly at `tbd_lo` needs before it is reported. Something the model scores higher is reported proportionally sooner. This decides *how long a faint target waits*, not what is accepted — anything the model keeps seeing is reported eventually regardless (measured: 2 → 20 changed the box count by 19%). |
| `tbd_lo` | 0.02–0.50 | **how faint a hint is worth collecting** — the confidence the model is actually run at, and the level at which a hint counts as no better than clutter. **This is the setting that decides how much is accepted** (measured: 0.15 → 0.30 halved the box count). Clamped to never exceed `conf`. ⚠️ People sit at the bottom of the stock model's range: raising this above ~0.25 deleted *every* person on the day clip. Re-measure with the trained model. |
| `tbd_decay` | 0.1–3.0 | score subtracted per tick with no observation. This is what kills flicker: a candidate seen every third tick nets negative and dies. |
| `tbd_max_miss` | 1–10 | consecutive missed ticks before a candidate track is abandoned |
| `cadence` | 1–8 | run the detector every Nth captured frame (1 = every frame; 4 ≈ 15 Hz at 60 fps capture). The integrator's association gate scales with the resulting tick rate automatically. |
| `max_dets` | 1–512 | cap on detections per frame |
| `nms` | 0.10–0.90 | box-merge IoU (lower = merge more; also merges a box mostly inside a higher-scoring one — collapses the multiple boxes a big/close object produces) |

### Frozen motion-worker knobs
Retained for a possible revival only — see the module README. `motion=0` by default.

| knob | range | meaning |
|---|---|---|
| `motion` | 0/1 | frozen motion worker on/off |
| `mot_method` | 0/1 | reference method: **0** = background-subtraction (median of `mot_window_s`), **1** = frame-difference (vs `mot_baseline_s` ago, default) |
| `mot_baseline_s` | 0.25–5 s | frame-diff baseline: difference against the frame this many seconds back |
| `mot_k` | 1–30 | MAD threshold multiplier (noise floor above the median diff) |
| `mot_window_s` | 1–60 s | background-subtraction window |
| `mot_persist` | 1–5 | confirmation strength within the ~1 s M-of-N window |
| `mot_down` | 1–4 | spatial downscale. **1 = native**; higher blinds small targets. Changing it rebuilds the worker. |

> Pitfall: the motion reference is compared in the current frame, so it is correct only on
> a static/holding mount until real ego-motion is wired behind `stabilize()` (`-E` ECC now).
> Pitfall: at native the worker floods on daytime wind-blown foliage — genuine motion, not
> noise, and no `mot_k`/baseline setting removes it without also deleting the far targets.

## Output — `airpoc.det_wire` (recorder tap)
Best-effort shm publisher (protocol v1), byte-verbatim `/stream` JSON, 16 slots.
No-op if it can't be created — a recorder-less system runs unchanged.
