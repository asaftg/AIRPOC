# Radar detection — what we tried, and what it cost

Written 2026-07-21, from the session that built `slowdet.c` + `fuse.c`.

Every entry below was **built and measured**, not reasoned about. Most of them
failed. They are recorded because each one looks like an obviously good idea
from a standing start, and re-running them costs a day each.

Fixtures referenced are the ones in [`TEST_CORPUS.md`](TEST_CORPUS.md): positives
`walk`, `longnight` (night human out to ~300 m); negatives `static`, `c16`,
`coldboot` (nothing should ever fire).

---

## A. Making the detector see a slow walker

### A1. Pure position-only chaining — invented tracks
Earlier attempts (`chain_back.py`, `slow.py`) linked echoes by position alone,
with no Doppler at all. They produced large numbers of confident-looking tracks
that were not there. This is why the shipped detector lets Doppler *lead* a hop.

### A2. "Doppler is advisory" — exploded
The shipped detector will not chain **through** an echo whose Doppler reads below
0.5 m/s. Measured on the night walker, that reading is wrong **62 % of the time**:
the echo is present in 97 % of frames (longest gap 0.11 s), but a ~1 m/s human
sits in the zero-Doppler bin. So the detector was throwing away most of the
walker past 240 m. The obvious fix — treat "Doppler ~ 0" as *unknown* rather than
*static*, and let the persistence map and the travel test do the filtering:

| | shipped | Doppler-advisory |
|---|---|---|
| longnight tracks | 31 | **504** |
| longnight declared dots | 11 217 | **585 188** |
| **static** (must be 0) | **0** | **47 tracks / 41 883 dots** |

**Why it fails:** a 1 m/s walker moves ~2 m in the 2-second declaration window —
**less than one 2.6 m range bin**. Inside 2 seconds a walker is *physically*
indistinguishable from clutter by position alone. Doppler was the only thing
separating them, so removing it as a gate removes the discrimination entirely.

### A3. Two-speed declaration (longer window for the Doppler-unknown) — still failed
Natural follow-up: keep the fast 2 s bar for echoes with believable Doppler, and
make the Doppler-unknown ones prove themselves over **6 s**, requiring 4 m of net
travel (>1 range bin) and 20 hops.

Result: **static 172 tracks / 25 383 dots.** Still catastrophic.

**Why it fails:** with ~420 points per frame, ~98 % of them CFAR false alarms, a
position chain can **walk** across the clutter field — hopping from one noise
echo to the next, drifting well past 4 m over 6 s without any real object. Pure
position has no coherence constraint that clutter cannot fake. Doppler
continuity (consecutive echoes sharing a speed reading) *is* that constraint.

### A4. Corroboration instead of chaining — works, different trade-off
`trails.py` counts *corroborating echoes in a 6-second window* rather than
requiring an unbroken chain. Measured: **static 54, c16 9** declared (essentially
clean) and **16 002** on longnight — more than the chaining detector's 11 217.

This is a genuinely viable alternative and the reason is instructive: chaining
shatters when 62 % of a target's echoes are unusable, because it needs an
unbroken sequence. Corroboration only needs 6 votes out of ~158 frames, so 38 %
usable is plenty. **Not shipped** only because the chaining detector was already
validated end-to-end; if the far-range walker needs more coverage, this is the
first place to look.

---

## B. Gates that quietly made things worse

### B1. SNR-gated hop reach instead of range-gated
Letting the look-back window grow with echo strength rather than range *sounded*
more principled. It added far-range vehicle noise: **37 tracks where the
range-gated version gave 8.** Reverted; `hop_reach()` is range-gated.

### B2. Declaring too early (a mis-port that cost 8× the far noise)
The Python reference marks a dot "confirmed" at `L >= NEED` (6 hops), and then
**additionally** requires the thread to *hold NEED confirmed dots*. A chain
therefore has to survive ~2×NEED hops. The first C port implemented only the
first gate. Effect on longnight beyond 400 m: **123 declared dots vs the
reference's 16.** Fixed by declaring at `DECL_L = 2*NEED-1 = 11`, which restored
it to 3. If far-range noise ever reappears, check this constant first.

---

## C. Crossing (tangential) movers — built, measured, deliberately OFF

A target crossing the beam has almost no radial Doppler, so the detector is blind
to it. Two attempts:

| version | static (must be ~0) |
|---|---|
| naive: drop the Doppler gate, judge motion by position incl. cross-range | **12 438 dots** (~15 per frame) |
| + monotonic-sweep guard | **958** (13× better) |
| bar to ship | ~0 |

**Why the naive version fails:** static clutter jitters ±1–2° in azimuth. At
100 m that fakes ~1.75 m/s of cross-range "motion" — above the 0.5 m/s travel
bar. Worse, the jitter smears the object across azimuth cells so the persistence
map never marks it static.

**The guard that helped:** a real crosser's bearing sweeps *monotonically away*
from where its chain started; jitter oscillates around it. Requiring each
Doppler-less hop to grow `|az − az0|`, plus a 4° net sweep to declare, cut the
leak 13×. Not enough. **`slowdet_set_tangential()` exists and defaults OFF.**

Next ideas, untried: range-band persistence (mask a whole range bin that is
clutter-occupied across many azimuth cells), or a bearing-rate consistency model.

---

## D. Elevation — the axis the radar is bad at

The antenna is spread sideways but barely stacked vertically, and spread is what
buys angle accuracy. Measured target box half-extents: **azimuth 0.6°**
(genuinely the target) vs **elevation 3.5° median, 11.2° at p90** — against a
vertical half-FOV of only 8.96°.

### D1. Sizing the box by the elevation spread — box taller than the picture
Honest, and useless. At p90 the drawn box exceeded the entire frame height. The
elevation spread measures *the sensor's noise*, not the target. Shipped instead:
**width from the measured azimuth spread, height capped at 1.5°.**

### D2. Heavy multi-second averaging — wrong for an aerial seeker
Elevation noise is random per frame, so a long average is tempting: a 2 s window
at 26 Hz is ~50 samples, ~7× noise reduction. **This reasoning came from the
ground-walk recording and does not hold for the product.**

Elevation angle changes at roughly `v_z / R`. A 15 m/s climbing drone at 200 m
sweeps **~4 °/s** — the entire noise budget in half a second — and **~17 °/s** at
50 m. The usable window is `(accepted blur × R) / v_z_max`, which is **~0.1 s at
200 m**, giving only ~1.7× noise reduction. A multi-second average would smear a
real manoeuvring target, worst exactly in the terminal phase. Hence the shipped
**range-scaled** window (0.6 s at 200 m, clamped 0.3–1.2 s) rather than a fixed
long one.

### D3. Median vs mean vs echo-weighted — not the lever
Measured residual at a 0.1 s window: **mean 1.99, median 2.15, echo-weighted
2.02** — all within 8 %. Choosing among them is not where the win is. Median
shipped anyway, for outlier rejection against the 11° flyers.

### D4. A light per-frame smoother — too weak
An adaptive one-pole whose rate followed the echo count only moved the
frame-to-frame jump from 1.15° to 0.61°. The trailing median at 0.6–0.8 s takes
the **worst-case** jump from **16.3° to 0.66°**. That is what shipped.

---

## E. Assembling tracks and boxes

### E1. Coasting/stitching the fragments — solved almost nothing
The slow detector's output on longnight is 30 fragments. Stitching them by
coasting across gaps merged **30 → 28**. The coaster bridges 4 s; the real gaps
are **13 s and 32 s**. Stitching is the wrong tool for a 32-second hole — that is
a detection-coverage problem, not an assembly problem.

### E2. "Suppress S where F exists" — produced the visible double boxes
Hiding a slow target only where the fast tracker had ≥3 supporting echoes
recovered just 128 of 798 suppressed samples, and **146 samples were kept while
an F box sat on the same object** — drawing two boxes on one target, the worst of
both. The correct answer is **merge**: emit one box per object, fast
authoritative, slow only ever adding. That is what `fuse.c` does.

### E3. Full-rate slow targets with per-frame elevation — box slid off the target
Publishing the slow detector at the full 26 Hz (it had been throttled to 4 Hz by
a 0.25 s binning left over from graphing) made the box jump off the walker into
empty road, because each frame's elevation carried the raw ±3° noise. Fix:
**split the rates** — range and azimuth at full rate, elevation from a short
trailing window. This is the same principle as D2, applied per axis.

---

## F. Analysis mistakes that produced confident wrong answers

Recorded because they were caught only by cross-checking, and they would have
shipped bad conclusions.

### F1. Re-zeroing a detector's timestamps
`dots[:,0] -= dots[:,0].min()` slid the detector's output so it *started* at
zero, dragging targets from 20–34 s into a "first 10 seconds" analysis. This
produced a graph showing a track that was not there, and a "mechanism" conclusion
built on echo evidence sampled at the wrong time. **Made twice in one session.**
Detector output must stay on the recording's clock.

### F2. Concluding bias from a table sorted by the bias
Sorting tracks by "claimed speed − actual speed" and reading the top rows gave
"every track overstates its speed by 2–3×". Across three recordings the **median
claimed/actual is 1.02–1.22×** — the tracker's speed is essentially right. Only
~2–4 % of tracks are the pathological case.

### F3. Scoring coverage against invented ground truth
Hand-guessed anchor points for the walker's path put it at 175 m when the
detector's own validated threads had it at 230 m. Use the operator's marked
path (`eval.py` carries it) — not a reconstruction.

---

## G. Findings about the sensor and the existing tracker

Not our code, but measured here and worth writing down.

### G1. A static tree published as a 3.3 m/s mover
The tracker emitted a target for 3.7 s at 319 m whose azimuth never moved
(12.8° → 12.8°) and whose position was static in both axes. Its published record
said `vx = −3.268 m/s`. **The min-velocity gate is working — it is being fed a
fabricated velocity.** The cluster holds 152 echoes, 93 % reading exactly
0.00 m/s, but ~7 % carry junk Doppler up to **40.88 m/s** (the fold limit).
Radial velocity survives that (the bulk dominates); **cross-range velocity can
only be inferred from how Doppler varies across the cluster's bearing spread**,
which is exactly what the outliers destroy. Rate: ~2–4 % of tracks across three
recordings. Cheap fix, untried: cross-check claimed velocity against actual
displacement over a couple of seconds.

### G2. The "380 m wall" is terrain, not a range limit
In `REC 2026-07-20 12:51` the point cloud stops dead at ~380 m for 42 seconds,
then opens to **498 m** once the gimbal pans — same clip, same configuration,
same maximum as the night recording. Line-of-sight blockage. Do not read a stable
maximum range off a single pointing.

### G3. Highway vehicles at 300–350 m are under the detection floor
EO reports 650 vehicle detections; radar produces 2 short tracks in 29 s. The
band is not blocked (47 echoes/frame) but **98 % of those echoes are transient**
CFAR false alarms. Control test — the identical persistence test run at a bearing
15° away, where EO sees nothing:

| | at the EO vehicles | empty control bearing |
|---|---|---|
| echo present | 73.0 % | 77.9–80.4 % |
| still there next frame | 36.7 % | 39.9–45.7 % |

**Ratio 0.86×** — the radar sees no more at the cars than at empty sky. Nothing
above the point cloud recovers these; it is a fusion case, and EO carries them.
(Caveat: assumes the EO and radar azimuth zeros agree; a large uncorrected
boresight offset would weaken this, though not by enough to change the verdict.)

---

## How to re-run any of this

The offline harness builds against the real `RadarPoint`/`RadarTarget` types and
replays the fixture corpus:

```
cd radar/tools && make slowdet_harness
./slowdet_harness ../../fixtures/static.bin      # negatives: expect ~0
./slowdet_harness ../../fixtures/longnight.bin   # positive:  expect ~4538
```

See [`SLOWDET.md`](SLOWDET.md) for what the shipped detector does and
[`VALIDATION.md`](VALIDATION.md) for the corpus gates.
