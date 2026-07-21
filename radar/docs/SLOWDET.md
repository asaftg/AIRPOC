# The slow detector, the fuse stage, and elevation conditioning

## Why there are two detectors

`cluster.c` decides what is a target **within each frame**: it needs enough
points close together, then confirms the track over a few frames. That works
well for anything close or bright, and it is what we ship as the main tracker.

It cannot hold a target that is barely there. Measured on the recordings:

- A car at 300 m in daylight returns an echo in only about **60 %** of frames,
  usually a **single** point. Cluster's own successful targets are backed by a
  couple of points and are present ~80 % of the time. A single point that
  disappears four frames in ten never forms a cluster and never survives
  confirmation, so the car simply never appears.
- The night walker past ~240 m behaves the same way.

`slowdet.c` attacks the same data from the other direction. Instead of asking
"are there enough points **right now**", it asks "has this faint echo been
**walking a sensible path** for the last couple of seconds". It links echoes
frame to frame over short bounded hops and declares a target once the chain is
long enough and has actually travelled. One point per frame is plenty, and gaps
are tolerated. That is precisely the regime cluster cannot reach.

They are complementary, not competing. Cluster is authoritative where it works.

## How slowdet decides

- A hop links a recent echo to a new one if they are close in range and
  bearing, and their speed readings agree. Doppler only ever **leads** the hop —
  the speed we publish always comes from **position over time**, never from the
  ambiguous Doppler reading.
- Every hop is short, so a chain bends freely: a turning car is just a curving
  chain. There is no long straight-line assumption.
- A cell of the picture that is lit over and over is the static world (a wall, a
  parked car, a tree). Echoes there are excluded, which is what keeps empty
  scenes silent.
- A chain is declared only once it has enough hops over enough time **and** has
  physically travelled. Something that jitters in place is rejected.

## The fuse stage

`fuse.c` produces the target list that leaves the module.

- **One box per object.** Cluster's targets go out as-is. A slowdet target is
  emitted only where no cluster target already sits at the same range and
  bearing. Fuse can only ever *add* to cluster's list, never remove from it.
- Slowdet ids start at **1000** so the two sources are distinguishable on the
  wire; everything stays class-less, as the module contract requires.

## Elevation conditioning

The radar measures range and bearing well and **up/down badly** — about
**3 degrees** of noise typically and **11 degrees** at the tail, because the
antenna is spread out sideways but barely stacked vertically. Spread is what
buys angle accuracy, so there is very little vertical accuracy to have. No
processing invents a measurement that was never taken.

What we do about it, applied to cluster and slowdet targets alike:

- Range and bearing are left completely alone and stay at full frame rate.
- The published elevation is a **median over a short trailing window**. The
  window **scales with range** (0.6 s at 200 m, clamped 0.3–1.2 s) because the
  up/down angle of a distant target changes more slowly for the same real
  motion, so it can be averaged harder for the same lag.
- A **physical rate limit** clips any reading implying more than 20 deg/s of
  vertical motion before it can enter the window, so one wild frame cannot throw
  the box.
- The **vertical box height is capped** at 1.5 degrees. The raw vertical spread
  of the echoes reflects the sensor's noise, not the target's size — drawing it
  produces a box taller than the whole camera frame.

Note this removes **jitter, not bias**. A repeatable offset is calibration's
job, not the filter's.

### Why the window cannot simply be made long

This is an aerial seeker: targets are not at our level and do not crawl. The
up/down angle changes at roughly (vertical speed ÷ range), so a 15 m/s climber
at 200 m sweeps ~4 deg/s — the entire noise budget in half a second, and worse
as it closes. A multi-second average would smear a real manoeuvring target and
would be worst exactly in the terminal phase. Hence the range-scaled window
rather than a fixed long one.

## Measured

Fixture corpus, via the offline harness against the real `RadarPoint` types:

| | result |
|---|---|
| negatives — static / c16 / coldboot | 0 / 0 / 49 declared (0.003 per frame) |
| positives — walk / longnight | 11274 / 4538, identical to the validated reference |
| elevation frame-to-frame jump | 4.06 deg → 0.17 deg |
| cost on the Orin | 446 us/frame = 1.2 % of the 26 Hz budget |

Live on the bench after deployment: a vehicle held at ~157 m closing at ~12 m/s,
elevation steady within 0.02 deg across the pass.

## Known gaps

- **Crossing (tangential) movers.** A target moving across the beam has almost
  no Doppler and is not caught. A position-only mode was built and measured: on
  pure clutter it still declared ~1.2 false targets per frame even after a
  monotonic-sweep guard cut the leak 13×. It is **not** enabled, and should not
  be until that reaches ~zero.
- **Targets under the detection floor.** On the south-highway recording the
  vehicles produce no echo that stands out from the CFAR noise at all — a
  control test at a bearing 15 deg away scored the same as the vehicles
  themselves (ratio 0.86×). Nothing above the point cloud can recover those;
  that is a fusion case, and EO carries them.
