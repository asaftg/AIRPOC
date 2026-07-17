# Radar tracker — parameters, decisions & tuning

The clusterer is a **temporal multi-target tracker** (`src/cluster.c`). This doc
lists every knob and every fixed internal number, what it does, and **why it is
set where it is**.

> Pitfall: **every value here was tuned against a single recording** (the garage
> + street session). They are a validated *starting point*, not universal
> constants. Re-tune as more recordings arrive — see "Re-tuning" below.

## Pipeline

```
chip CFAR → point cloud (+per-point SNR)
  → point gates (SNR / speed / FOV / elevation band)
  → moving channel  +  fresh-static channel (occupancy map)
  → predict + nearest-track association (velocity from position history)
  → M-of-N confirm → short coast / park-hold → spatial dedup → target boxes
```

Velocity comes from a least-squares fit of each track's own position history
(range-rate + angle-rate), **never** from the ambiguous Doppler — that is the
core design choice that makes the tracker class-agnostic.

## Where each parameter lives

- **Live** — `GET /ctl?...` (sliders / GUI), no restart, echoed in `/stats`.
- **Fixed** — `#define` in `src/cluster.c`; changing it means a rebuild.
- **cfg** — `cfg/awr2944P_ag.cfg`, applied on the chip; needs a radar power-cycle.

## Live knobs (the 9 on `/ctl` + `/stats`)

| Knob | `/ctl` key | Default | Range | Effect |
|---|---|---|---|---|
| Dedup radius | `eps` | 4.5 m | 0.5–50 | co-located tracks emit **one** box; ↑ merges more. |
| Min points | `minpts` | 2 | 1–20 | radar dots needed to start a track; ↑ = stricter, rejects sparse noise. |
| Min speed | `speed` | 0.7 m/s | 0–5 | Doppler motion threshold; slower dots ignored as static clutter. |
| Min SNR | `snrmin` | 16 dB | 0–60 | dot strength gate (static channel uses +3). Chip already floors at ~16. |
| FOV | `fov` | 90° | 5–90 | azimuth gate, input **and** emit; the radar's real AoA limit is ±30°. |
| Elev cap | `elmax` | 20° | 5–90 | elevation half-angle gate (radar-frame, symmetric). Default = the antenna's physical elevation beam edge — beyond ~±20° the 2-row array has no real gain, so reports there are angle-noise/multipath. Gimbal-safe (the beam moves with the radar). 90 = off. |
| Merge gate | `doppler` | 1.2 m/s | 0.5–20 | two co-located tracks merge only if their speeds agree within this. |
| Confirm | `confirm` | 3 | 1–6 | M-of-N fast-confirm hits (window N = M+1). ↓ = appears faster, more false. |
| Coast | `coast` | 0.4 s | 0–3 | how long a confirmed track survives a dropout. |
| Park hold | `park` | 15 s | 0–60 | how long a moved-then-stopped track is held. |

## Fixed internal numbers — the decisions

All **empirical** (tuned against the recording) unless marked *physical*. These
are the "why 8-of-12 and not 8-of-20" answers.

| Number | Value | Why this value |
|---|---|---|
| Fast-confirm window | N = M+1 | at most **one miss** allowed → a clean mover confirms almost immediately; keeping it tight is what stops noise from confirming fast. |
| Slow/static confirm | **8 of 12** | backup path for a flickery or parked target the fast path would miss. 12 frames ≈ 0.5 s (a sensible recent window); 8/12 ≈ two-thirds = "clearly present more than a coin-flip → real, not noise." 8-of-20 would confirm sparser things, slower, and admit more noise. |
| Jitter gate (fast / slow) | 2.6 m / 1.8 m | a track only confirms if its measured position is **consistent** frame-to-frame; noise jumps around, a real target does not. |
| Occupancy learn (warm / run) | 0.10 / 0.0003 | how fast a static return becomes "background." After the 8 s warm-up a new static needs ~17 s+ to fade in, so an **idling car stays "fresh"** and keeps its box while true scenery is ignored. |
| Occupancy decay / free | 5e-5 / 0.35 | a cell counts as historically empty below 0.35; slow decay so the map is stable. |
| Association gate | 6 m range, 4 m cross, +0.45·speed, ×miss-growth | how far from its prediction a track will claim points; grows with speed and consecutive misses. |
| Velocity-fit window | 0.9 s, min span 0.22 s | least-squares slope of position history → range-rate / angle-rate. |
| Position blend | r 0.55, az 0.70, el 0.10 | measurement-vs-prediction smoothing; az tracks faster, **el is heavily smoothed** because the 2-row array's elevation is noisy. |
| Seed link / guard | 5 m, 3.5 m cross / 5 m, 2.5° | how dots group into a new track; don't seed on top of an existing one. |
| Min range | 3 m | *physical*: ignore returns closer than 3 m. (The old fixed −9°…+2.5° elevation band was removed 2026-07-10 — level-mount-only tuning, wrong on a gimbal and blind to airborne targets; elevation gating is the `elmax` knob only.) |
| Emit range / FOV | 500 m / the FOV knob | full radar coverage — no artificial clamp. |
| Frame rate assumption | 26 Hz (`TRK_FPS`) | converts the coast/park **seconds** knobs to frames; matches the A/G profile. |

## Liar latch (fixed, `LIAR_*` / `WALK_*` in `src/cluster.c`)

Kills the far-clutter ghost class seen on the 2026-07-13 recordings: a
confirmed track past 100 m that keeps a full 5 s of position history yet whose
points almost never carry usable doppler (its "motion" comes from the tracker
re-latching onto nearby junk, not from anything actually moving). A real mover
past 100 m carries claimed doppler nearly every frame — measured on the T7
night walker out to 306 m. The measured ghost only touches doppler in short
soup bursts (0.04–1.4 s of coverage inside a full 5 s window) with wild values
(1.2–31 m/s frame to frame).

How it works, in plain terms: every frame that brings fresh doppler, the track
is asked "over the last 5 seconds, how much of your history actually carried a
doppler story?" Starved-but-claiming frames add a strike, healthy-coverage
frames remove one, and 13 net strikes **latch** the track as a liar. A latched
track keeps living and keeps claiming its points — so its junk cannot re-seed
a fresh ghost — but it never reaches the wire again, and when it dies it
leaves no graveyard credential for a successor to inherit.

| Number | Value | Why |
|---|---|---|
| Judge floor | 100 m (`LIAR_R_MIN`) | below this, multipath makes claimed doppler soup for **real** targets too; near tracks belong to the flood + consistency guard. |
| History span | ≥ 4 s (`LIAR_SPAN_MIN`) | starvation only counts on a track with a full window — a young track is not judged. |
| Coverage bar | 1.5 s (`WALK_COV_MIN_S`) | measured: real movers cover most of the 5 s window; the ghosts never reach 1.5 s. |
| Claim floor | 1.2 m/s median (`WALK_DOP_MIN`) | a radially-quiet target (crosser, stopped) claims ~nothing and is never judged. |
| Latch | 13 net strikes (`LIAR_KILL`) | ~0.5 s of sustained starvation; hysteresis (strike/pay-back) mirrors the consistency guard. |
| Latch, not kill | — | killing was tried and **raised** radar4 emissions 43%: each kill spawned a successor-ghost chain and reshuffled emit dedup scene-wide. The latch keeps tracker dynamics bit-identical outside the liar itself (validated: 10 of 12 corpus fixtures emit bit-identically with it on). |

Validation (2026-07-16 corpus): the radar4 far-clutter ghost (450 emitted
frames at 142–178 m) goes to **zero**; the radar4 walker, the whole T7 night
walk (turnaround included), V2DAY, T2, T4, T5 and all noise fixtures are
bit-identical to V2.

## Reflection-copy suppressor (fixed, `REFL_*` in `src/cluster.c`)

Antenna sidelobes show a bright mover a **second time**: same range (lockstep,
bin-identical), same signed speed, 10–70° away in azimuth (measured on the
2026-07-13 garage recording — the walker and the parked car each drag such
copies). The suppressor watches every confirmed track for this signature
against the frame's stronger emitted targets and lets **time** convict:

- a genuine second target crossing another's range looks identical for a
  moment — measured on the T1/T2 crossing pairs it stays co-ranged ~1–2 s,
  and naive same-frame suppression wrongly ate 94/29 of their frames;
- a reflection shadows its source for its **whole life**.

While a track is in a shadow but not yet convicted, it **still emits**, marked
`"sus":1` on the wire (fusion can hold fire; the radar hides nothing). A shadow
held ≥ 2.5 s convicts: the copy stops being published (the track keeps living
so its junk cannot re-seed). Once one copy has served full time in a source's
shadow, the source is a **proven mirror-breeder**: its later copies convict on
sight — each ghost episode is shorter than the dwell, but the source geometry
already proved itself (this is what catches the short-lived copy chains).

| Number | Value | Why |
|---|---|---|
| Co-range gate | 5 m (`REFL_R`) | measured copies are range-bin-identical to the source. |
| Co-velocity gate | 1.5 m/s SIGNED, wrap-aware (`REFL_DV`) | V2DAY's opposite-direction pairs (−4.3 vs +4.4 m/s) meet in range but never match — sign protects them. |
| Separation floor | max(4.5 m, 10° arc) (`REFL_SEP_M/_DEG`) | closer than this is the spatial dedup's territory (one body split in two); a shadow is far in azimuth yet glued in range+velocity. |
| Conviction dwell | 2.5 s (`REFL_SHADOW_S`) | T1/T2 real crossing pairs max out at ~1.7 s of shadow (measured); the T1 slowest real shadow-dweller (3.46 s) emits nothing between 2.5 and 3.46 s, so 2.5 s cuts deeper into the mirrors at zero real cost. |
| Clear reset | 1.0 s continuous (`REFL_CLEAR_S`) | single-frame match blips (measurement noise) must not launder a copy; a real crosser separates and STAYS separated. |

Validation (2026-07-16 corpus): radar5 mirror population 453 → 137 emitted
frames (walker and car retained to the frame); T1/T2 crossing pairs zero
suppressed frames; T7/T4/V2DAY/T5 bit-identical.

## Walk guard (fixed, `WALK_*` in `src/cluster.c`)

Motion **verification**: a real mover's claimed doppler, added up over 5 s,
equals the range distance it actually walked; a multipath "breather" claims
motion its position never delivers. Per confirmed track the guard compares
that doppler integral (D) against the measured walk (dR), both robustified
(claims clamped against the window median, range steps capped against the
claim so association re-latch jumps don't count as motion).

- **Verified** (`mv_class:1`) — the stories match, or the track shows coherent
  cross-range motion with a sane radial story. The credential latches
  (`mv_ever`), survives track death via the graveyard, and a verified track is
  **never** walk-latched — a real target reversing through its turnaround
  (the T7 walker at 306 m) must survive.
- **Unverified-slow** (`mv_class:0`) — radially quiet or not enough claimed
  motion to judge (crossers, parked, faint). Never judged, always still
  emitted.
- **Suspect** (`mv_class:2`) — decidable and contradicted; a fail streak in
  progress. 13 consecutive decidable fails on a never-verified track LATCH it
  off the wire (it keeps living and claiming its junk — same mechanism and
  same measured reason as the liar latch: killing these tracks spawned
  successor-ghost chains that pushed radar4/V2DAY above their frozen
  baselines).

Key numbers: decidable needs median |claim| ≥ 1.2 m/s, |D| ≥ 3 m, ≥ 1.5 s
covered (`WALK_DOP_MIN/_D_MIN/_COV_MIN_S`); fails only count at r ≥ 20 m
(`WALK_R_MIN` — near-field claims are multipath soup); tolerance
1.5 m + 1 cm/m + 30 % of |D| (`WALK_TOL_*`).

Validation (2026-07-16 corpus): T7 loses only an off-corridor 130 m breather
(the return-leg corridor is at ~185 m at that moment); every corridor human
fragment and the 306 m turnaround untouched; T2 loses one 84-frame breather;
T6/noise stay zero; T1/T4/T5/radar4/radar5 bit-identical.

## LLR confirm path, far-only (fixed, `LLR_*` in `src/cluster.c`)

A second, evidence-accumulating way for a track **past 150 m** to confirm.
The standard M-of-N paths need a dense hit pattern inside a short window; a
far, faint target flickers, and its hits-with-gaps never line up. The LLR
score adds up likelihood instead: every hit adds "how much better than local
clutter does this measurement fit the track" (brighter return = tighter fit
= more credit; the local clutter rate is learned online per 50 m range ring),
every miss subtracts a fixed penalty. Reaching a score of 5.5 confirms, with
the same jitter gate and a reduced moving-rate floor.

Far-only is the re-add fix: the unrestricted quad-era path once bred a
phantom class at 130–138 m; below 150 m the strict M-of-N floor stands alone.
Per-hit credit is capped (+1.5) so an empty ring cannot let one bright
multipath coincidence confirm in two hits.

Honest validation note (2026-07-16 corpus): calibration holds — noise
fixtures stay at zero, T1 gains 15 far frames, nothing exceeds any baseline —
but this path did NOT move the T7 return-leg re-acquire it was hoped to fix.
Measured in the 34 s re-acquire gap: 928 distinct tentative tracks die of
4-miss runs at the night walker's 16 dB detection duty vs 4 that confirm at
all. The binder is tentative death by misses (an association-continuity
problem), not confirmation latency.

## Guidance output filter (fixed, `OUTF_*` in `src/cluster.c`)

What the **wire reports** for each target is a smoothed output filter, not the
raw per-frame cluster median. Output-only: association, gating and lifecycle
never see it (validated — emitted counts and track-id sequences are identical
across the whole fixture corpus with the filter on).

| Number | Value | Why |
|---|---|---|
| Angle smoother | alpha-beta on (az, az-rate) + (el, el-rate), Benedict-Bordner beta = a²/(2−a) | steady angles for the gimbal; the rate state follows a constant-rate crosser without lag. |
| Alpha tiers | 0.35 / 0.20 / 0.12 at <20 hits / ≥20 / ≥60 hits + peak SNR ≥21 dB | a young or faint track follows its measurements; an established bright one smooths hard. Far-segment (>200 m) az step-std 0.466→0.150° (3.1×) on T7. |
| Range/vr filter | same alpha-beta + claimed-doppler median as a **direct** range-rate measurement (gain 0.35, gate 3 m/s) | doppler sign verified on this firmware = range-rate (outbound walker claims +1.8 m/s); the gate rejects folded/soup doppler. |
| Coast / park | rates held through misses; park-held track bleeds rates ×0.5/frame | coasting continues the trajectory; a parked box must not drift on a stale rate. |
| Re-acquire slew | output angles converge ≤3°/s + track rate after a coast (`OUTF_SLEW_DPS`) | the gimbal never sees a re-latch teleport; normal tracking follows the filter exactly (no cap). |

Re-add note (this was in the reverted 2026-07-13 quad): the original version
deduplicated emitted targets by comparing a raw candidate against the
**filtered** wire outputs — two views of the same scene that drift apart. The
dedup now compares raw track positions on both sides, which is exactly the
comparison the pre-filter tracker made (validated count-neutral corpus-wide).

## Doppler-native association — evaluated and NOT re-added (2026-07-16)

The quad-era 3-D association gate (range, azimuth, doppler; commit 8b277e0)
was re-applied on top of the ghost-killer stack and measured against the
frozen corpus before deciding. Verdict: **dropped.** With the reflection
suppressor active it still blew four fixtures far past their
never-exceed baselines — radar5 1.29 → 3.30 E/fr, radar4 0.69 → 1.59,
V2DAY 1.24 → 2.62, T1 2.87 → 4.30 — and it regressed T7's near-field
outbound coverage (<70 m) from 659 to 440 covered frames (−18.5 points,
reproducing the regression that got the quad reverted in the field). The
one gate it passed: T7 70–200 m coverage 3286 → 3898 frames. Holding a
track together by its doppler identity keeps real far tracks alive AND
keeps every junk track alive; the corpus says the junk wins. Far-leg
continuity should come from the LLR confirm path plus a patience-style
detector, not from widening what a track may claim. Do not re-introduce
without replay evidence on this corpus.

## Far-range patience detector (fixed, `PAT_*` in `src/cluster.c`, 2026-07-17)

Beyond ~200 m a walking person's echoes are too weak to confirm through the
normal path: measured on T7 at 285-295 m the tracker covered him 12% of the
time, and of 546 tentative tracks near the real human only 5 ever confirmed —
the binder is the **confirmation floors** (M-of-N hit rate, moving-rate
EWMA), not association. But a real person leaves a **consistent trail**: over
5 seconds his blips line up on one distance-line that moves exactly at his
claimed-doppler speed. The junk carpet (~185 points/frame) produced **zero**
qualifying trails in every recording of the corpus.

How it works, in plain English: the tracker remembers the last 5 seconds of
far moving points. Each frame, every far slow mover that no healthy track
owns is tested as the endpoint of a trail — at least 13 remembered points, at
most one per frame, spanning at least 3 seconds, each sitting where "this
point moving at its claimed speed" predicts, at the same bearing and the same
speed. If the trail exists, the track that covers it is confirmed and put on
the wire; if nothing covers it, a new confirmed track is seeded from it.

| Decision | Value | Why (all measured) |
|---|---|---|
| Chain gates | 1.5 m / 1.5° / 0.5 m/s, ≥13 links spanning ≥3 s, one link per frame | fixed, never miss-grown; noise made 6/13 at best across the corpus |
| Range floor | r ≥ 190 m — **documented invariant, not a tunable** | with the full safeguard stack, a floor-130 experiment now stays sane (T7 +14% band E where the walker really is, T2 lower, negatives 0) — but 190 is where the field problem lives and nothing below it was ever field-validated |
| Speed cap | anchors only up to 3 m/s | the starved class is slow and weak; every faster chain measured was a vehicle the normal path already tracks |
| Copy suppression | co-range ±5 m, signed co-doppler ±1 m/s fold-aware, >10° apart → the weaker chain dies; in-line (≤3°) co-doppler chains ROUTE onto the existing track out to 24 m | sidelobe mirrors and bounce/range-offset echoes must not be born as tracks |
| Static-complex veto | line CONTRAST: in the frames that light the chain's tube (±1.5°, ±7.5 m around the anchor's own motion line, same speed class), at least half must actually chain | a range-extended clutter band lends a point to any line but fails the gates in most frames; occupancy alone was measured killing the real longnight walker (his corridor idles at occ 0.33-0.88) while contrast separates cleanly |
| Reduced credentials | chain-confirmed tracks are emission-eligible but NOT graveyard-eligible and NOT deep-pass-latched until they independently earn the full guard streak | an injected mistake must not found a lineage |
| First-wire debounce | 8 frames; ×4 for grants in high-occupancy cells | grant-heads that die in 3-6 frames each burned a wire tid; the ×4 tier is the **T7 emission rationer** (see below) |
| Claim vet | while on chain credentials, the track only claims moving points within ±1 m/s of its own last measured claim | junk claims during a far target's 5-7 s fades poisoned the guard and killed the track (12 tids across one T7 leg); vetted, a fade is clean misses and the park lease carries the SAME tid to the target's return |
| Guard-kill deferral | a track with a live qualifying trail within the last 8 s may be unlatched but not killed | at 12% duty the guard's 2.5 s window reads fade-junk as incoherence; freshness is re-proven each hit frame and denied to tracks in guard/walk trouble (an always-open refresh bred an immortal wanderer, guard_bad 72) |
| Wire coast | far patience tracks emit through at most 1.5 s of misses | the E-budget knife edge: 3 s put T7 and longnight 2-3% over their frozen emission gates; 0.4 s starved the longnight band to 0.28 |

**The measured T7 trade.** T7's frozen never-exceed emission gate
(E/fr ≤ 0.811) permits ~7580 emissions; full far-band coverage plus the
scene's real second mover is ~8200 — **full T7 far coverage and the frozen
gate are jointly impossible**. With the high-occupancy probation (`PAT_WARM_OCC 4`)
the corpus passes 42/42 and longnight's far band reaches its target, at the
cost of T7's 250-306 m outbound band (0.15-0.22 vs 0.57-0.69 baseline).
Compiling with `-DPAT_WARM_OCC=1` flips the trade: T7 225-300 m outbound
coverage 1.00 and return re-acquire at 304 m — and T7's emission gate fails
at 0.97/fr. Choosing coverage over the frozen budget requires re-basing the
T7 baseline, a decision for the field, not for this commit.

There is **no live knob** for any of this. `/stats` carries `chains_active`
and `chains_confirmed_total` for live verification.

## Re-tuning (as we get more recordings)

The numbers above fit **one** scene. Different scenes (open field vs. clutter,
faster movers, drones, different mount height) will want different values. The
loop:

1. **Record** the new scene with the recorder (raw radar + EO together), so
   there's ground truth to score against.
2. **Score** each recording offline: replay it through the tracker and compare
   against the EO track of what was really there (detect rate, false rate,
   confirm latency, one-box-per-target).
3. **Adjust** live knobs first (they cover most cases); only touch the fixed
   numbers if a whole behavior is wrong.
4. **Re-validate across *all* recordings** before shipping a change — a value
   that fixes scene B must not regress scene A.

The offline tooling for step 2 is committed under `tools/`: `track_replay`
(replay a fixture through the shipping `cluster.c`), `walkout_score.py` (score
what the live tracker did in a recording), `parity_check.py` (C vs Python
reference), and `regression/` (fixture conversion + baseline fingerprints —
fixture list in [`TEST_CORPUS.md`](TEST_CORPUS.md)). See
[`ROADMAP.md`](ROADMAP.md) for what's next.
