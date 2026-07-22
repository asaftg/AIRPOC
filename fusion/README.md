# fusion - one target picture from two trackers

`fusiond` (:8096) joins the radar tracker stream (:8092) and the EO tracker
stream (:8095) into a single list of targets. When both sensors see the same
object, the published row carries the best of each: range and closing speed
from the radar, precise direction and class from the camera, under one stable
global id. Objects only one sensor sees are passed through in the same list,
so a consumer needs exactly one wire for the whole picture.

**Not in the critical path.** The EO-blind fallback chain (radar tracker ->
gimbal -> guidance) consumes :8092 directly and runs with fusiond dead.
Fusion adds a better picture when it is up; nothing depends on it being up.

## What it consumes

| input | where | what |
|---|---|---|
| radar tracker | `:8092/stream` (SSE) | class-less boxes, meters, ~26 Hz |
| EO tracker | `:8095/stream` (SSE) | classed tracks, radians, detector rate (operator-set, default ~15 Hz) |

Both connections reconnect on their own with backoff. A dead input degrades
the output to the surviving sensor; it never crashes or blocks the daemon.

## What it produces

| output | what |
|---|---|
| `GET /stream` | SSE, one frame per processed input frame (~41 Hz with both up), heartbeat at least 1 Hz |
| `GET /stats` | health, feed states, track counts, trim + estimator, knobs |
| `GET /ctl?k=v` | clamped knobs, applied live, always answers `ok` |
| `airpoc.fus_wire` | recorder tap, byte-verbatim `/stream` JSON, 16 slots x 128 KiB, `meta[6]={frame_id,n_fused,n_eo_only,n_rad_only,0,0}` |

The full wire contract is in [docs/INTEGRATION.md](docs/INTEGRATION.md).

## How it decides two tracks are the same object

Matching runs on every input frame, in angle space (the EO sensor frame; radar
angles get the mount trim added first):

- **Azimuth must agree.** Radar azimuth is good (~0.6 deg), EO azimuth is
  excellent - this is the primary test.
- **Elevation barely counts.** Radar elevation is physically coarse (two-row
  antenna), biased, and smoothed with lag, so it only separates targets that
  are far apart vertically - and it is ignored entirely while either side
  shows fast vertical motion (a climbing target would otherwise be vetoed by
  the radar's lagged elevation).
- **Motion must agree.** The radar's sideways angular rate is checked against
  the EO track's drift rate, and when the radar sees a verified mover closing,
  its closing speed predicts how fast the EO box should grow. A bush the
  camera model keeps calling "vehicle" never grows in step with a radar
  closure, so this also filters persistent camera false positives.
- **Size must agree.** The pair hypothesis carries the radar's range, so the
  camera box has a width in metres - it must roughly match the radar's box
  (softly: radar boxes are crude). A ~1 m person cannot marry a ~4.5 m parked
  car, whichever is momentarily nearest in angle.

A marriage needs both hits AND time: several co-observations over at least
~0.7 s, with the radar-vs-camera angle gap holding steady. A gap that slides
steadily is a pass-by (the radar target moving across a parked object), not
a match - a drifting candidate is refused, and a married pair whose gap
walks off the value it married at is divorced immediately, with that radar
track barred from marrying anyone for 1.5 s (otherwise a driving car chains
through every parked car on its path - seen in the field, now a regression
test). A one-frame blink never splits a pair: the drift checks use the
window median, which outliers cannot move. Radar targets outside the
camera's field of view are simply radar-only rows, never evidence against.

## What a fused row carries

Direction and box from the camera (falling back to the radar's, flagged
coarse, if the camera loses the target), range and closing speed from the
radar (held briefly and flagged `r_stale` if the radar drops it), the class
from a smoothed vote over the camera's labels, and the radar's quality flags
(`sus`, `mv`). The global id survives per-sensor track-id churn: if a sensor
re-acquires the same object under a new id, the row re-binds by continuity
instead of renumbering.

## Mount trim (radar <-> camera alignment)

Fusion owns the boresight trim between the two sensors: `trim_az`/`trim_el`
in degrees, added to radar angles. Set via `/ctl`, persisted to
`/var/lib/airpoc/fusion-trim.json` (or `./fusion-trim.json` when that path is
not writable), loaded at start. Shipped defaults are az +1.1 / el +2.2 deg
(the values measured on the current rig via the console overlay, 2026-07;
re-check on any mount change).

**Calibration (2 minutes):** with both sensors live, have one person or
vehicle move in view - somewhere OPEN: the estimator only measures isolated
targets (a radar mover with exactly one camera track anywhere near it), so a
row of parked cars teaches it nothing. Watch `trim.est_az_deg`/`est_el_deg`
in `/stats` - the median residual over those clean sightings, i.e. what the
trim *should* be.
Nudge `/ctl?trim_az=&trim_el=` until `tracks.fused` holds steadily and the
estimator agrees with the setting. The estimator only reports; it never
changes the trim by itself.

## Knobs (`/ctl`, all clamped, discover via `/stats knobs{}`)

| knob | default | range | meaning |
|---|---|---|---|
| `trim_az` | 1.1 | -10..10 deg | radar->camera azimuth trim |
| `trim_el` | 2.2 | -10..10 deg | radar->camera elevation trim |
| `gate` | 1.0 | 0.25..4 | match-gate scale (bigger = looser matching) |
| `confirm` | 3 | 1..8 | co-observations needed to declare a pair (confirm+1 of 2x confirm) |
| `divorce_s` | 0.6 | 0.2..3 s | sustained disagreement before a pair splits |
| `coast_s` | 1.0 | 0.2..5 s | how long a lost side keeps contributing |

## Build and run

```
make            # fusiond (pure C11, pthreads/libm only)
make check      # offline gates - synthetic core scenarios (including the
                # walker-beside-a-parked-car and car-sweeping-parked-cars
                # street scenes), TWO real recorded radar+EO pairs through
                # the real parsers, and a two-daemon loopback smoke.
                # Exits nonzero on any failure.
./fusiond -p 8096 -r 127.0.0.1:8092 -t 127.0.0.1:8095
```

`make check` needs no hardware. The recorded fixture in `tools/fixtures/` is
a 10 s cut of REC 2026-07-20 1251 (real radar_wire; trk_wire produced by
running the recording's real det_wire through the real eotrack core offline -
`tools/det_to_trk.c`). Longer runs against full recordings:
`tools/airec_to_jsonl.py <session>/radar_wire radar.jsonl` (same for det/trk
channels), then `tools/wire_replay radar.jsonl trk.jsonl`.

**Re-doing a recording's fusion after a core change:** `tools/refit.c` runs a
session's recorded radar+tracker wires through the CURRENT core and prints
the fus wire it would have published; `tools/airec_from_jsonl.py` packs that
back into an AIREC channel directory. Drop it into a hardlink copy of the
session (new sid, "REFIT " name in the manifest) and the replay library shows
the new behaviour against the same video.

> Pitfall: `tools/fake_feeds` serves a synthetic scene for the smoke test on
> non-production ports. Never point it at the production ports or run it in
> front of an operator.

## Layout

```
src/config.h        geometry, pools, sigmas, knob defaults/limits
src/core.c/.h       the fusion core: matching, pair lifecycle, global ids,
                    fused-state composition (pure, allocation-free, clock-free)
src/emit.c/.h       wire JSON serializer
src/rad_parse.c     radar wire parser (skips the point cloud entirely)
src/trk_parse.c     EO tracker wire parser
src/sse_client.c    SSE consumer with reconnect/backoff (two instances)
src/http.c          :8096 server (/stream /stats /ctl)
src/trim.c          trim persistence
src/main.c          daemon shell: feed callbacks, heartbeat, recorder tap
tools/              offline gates + bench tools (see Build and run)
systemd/            airpoc-fusion.service + install.sh
tap/airpoc_tap.h    vendored recorder tap header (do not edit)
```

## Performance

The matching math is negligible (worst case 64x64 gate checks per pass). The
real cost is string work, and it is designed down: the radar frame's point
cloud (~50 KB of JSON per frame) is skipped during parsing, and the output is
one single-pass serialize. Measured in `make check` on the recorded fixture:
~18 us per pass average (WSL x86); budget 300 us per pass, enforced by the
gates. On-target numbers get measured at the Jetson bench validation.
