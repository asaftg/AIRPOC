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
| `meta[4]` | illuminator state: `on | present<<1 | power<<8 | fov10<<16` |
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
   {"src":"app","cls":"drone","conf":0.83,
    "px":[912.4,301.2,14.0,9.5],"ang":[0.0553,0.0698,0.0040,0.0027]}],
 "movers":[
   {"src":"mot","age":7,"conf":0.60,
    "px":[120.0,88.0,6.0,5.0],"ang":[-0.1725,0.1311,0.0017,0.0014]}]}
```

- `px` = `[cx, cy, w, h]` in full-resolution pixels.
- `ang` = `[az, el, w, h]` in **radians**: `az = (cx - 719.5)·ifov`,
  `el = (543.5 - cy)·ifov` (az +right, el +up), `ifov` from the lens
  (default 287.5 µrad/px at f=12 mm — set with `-f`/`-i`). Angle is what fusion
  consumes; the camera has no range.
- `src`: `"app"` = appearance model, `"mot"` = motion worker. Where the two
  overlap the model box wins and the mover is dropped → **one box per target**.
- `cls`: `human` / `vehicle` / `drone` for model boxes; omitted for unclassified
  movers. `age` present on movers only (persistence age).
- `t_src_ns` is the frame-correlation key (see input table — driver clock, not
  CLOCK_MONOTONIC). `latency_ms = (t_out_ns - t_pub_ns)/1e6` is the detector
  pipeline time (EO-publish → we emit); `t_pub_ns`/`t_out_ns` are both
  CLOCK_MONOTONIC so the diff is valid.

- `cls` values from the stock COCO model are `human` (person) and `vehicle`
  (car/bus/truck only — not bicycle/motorcycle, which aren't target vehicles);
  `drone` arrives with the trained model. With no engine loaded, `model` is
  `"none"` and `dets` is empty, but `movers` still populates (motion runs regardless).

## Output — `GET /stats`
```json
{"version":"0.5.0","ifov_urad":287.5,"img":{"w":1440,"h":1088},
 "tap":{"connected":true,"fps":60.0,"gaps":0,"drops_cum":0,"frame_id":184223},
 "det":{"active":false,"fps":0.0,"model":"none","precision":"",
        "infer_ms":{"p50":0,"p95":0},"e2e_ms":{"p50":0,"p95":0}},
 "motion":{"active":false,"fps":0.0,"stab_fail_pct":0.0,"candidates":0},
 "knobs":{"conf":0.35,"cadence":4,"motion":1,"max_dets":128,
          "mot_k":6.0,"mot_window_s":15.0,"mot_persist":3,"mot_down":1,
          "mot_method":1,"mot_baseline_s":2.0}}
```
`det.active` is true once an engine is loaded (`-e`); `motion.active` is true once
the motion thread has processed a frame. `candidates` is the raw mover count
before overlap suppression.

## Output — `GET /ctl?k=v&...`
Sets live knobs; absent params keep their value; all clamped server-side;
replies `ok`.

| knob | range | meaning |
|---|---|---|
| `conf` | 0.05–0.95 | detection confidence threshold |
| `cadence` | 1–8 | run **both** the model and the motion worker every Nth captured frame (1 = every frame; 4 ≈ 15 Hz at 60 fps capture). One rate drives both detectors; the tracker's association gate scales with it. |
| `motion` | 0/1 | motion worker on/off |
| `max_dets` | 1–512 | cap on detections per frame |
| `nms` | 0.10–0.90 | box-merge IoU (lower = merge more; also merges a box mostly inside a higher-scoring one — collapses the multiple boxes a big/close object produces) |
| `mot_method` | 0/1 | motion reference method: **0** = background-subtraction (median of `mot_window_s`), **1** = frame-difference (vs `mot_baseline_s` ago, default). bg-sub is best on a stable scene but absorbs slow/near-stationary movers; frame-diff catches those and is cheaper. Both kept for the EO-tracker phase to choose. |
| `mot_baseline_s` | 0.25–5 s | frame-diff (method 1) baseline: difference against the frame this many seconds back. Longer catches slower/farther targets (more displacement); too short misses them. |
| `mot_k` | 1–30 | motion MAD threshold multiplier (noise floor above the median diff) |
| `mot_window_s` | 1–60 s | background-subtraction (method 0) window: how far back "normal scene" is modelled. Short adapts fast; long is smoother but slower to forget a stopped object. |
| `mot_persist` | 1–5 | confirmation strength = fraction of the ~1 s M-of-N tracker window a mover must hit before it's reported (rejects sparkle/twinkle) |
| `mot_down` | 1–4 | motion spatial downscale. **1 = native** (resolves the far/small movers the net exists for); higher is cheaper but blinds small targets (÷4 collapses a far human to ~3 px). Changing it rebuilds the worker (reference re-warms). |

The motion worker runs on the **same `cadence`** tick as the appearance model (one rate
for both). Raise `cadence` toward 1 as a target closes; searching at range can run slower.

> Pitfall: the motion reference is compared in the current frame, so it is correct only on
> a static/holding mount until real ego-motion is wired behind `stabilize()` (`-E` ECC now).
> `motion=0` by default for that reason.
> Pitfall: at native the worker floods on daytime wind-blown foliage — genuine motion, not
> noise. No `mot_k`/baseline setting removes it without also deleting the far targets; it's
> the **EO tracker's** job (keep translating tracks, drop oscillating ones).

## Output — `airpoc.det_wire` (recorder tap)
Best-effort shm publisher (protocol v1), byte-verbatim `/stream` JSON, 16 slots.
No-op if it can't be created — a recorder-less system runs unchanged.
