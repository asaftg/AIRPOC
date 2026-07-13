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
(range-rate + angle-rate). Since 2026-07-13 the claimed doppler (sign-verified
= direct range-rate on this fw) is a SECOND, fold-aware velocity identity: it
gates association (`DN_*`), verifies motion (walk guard), and feeds the wire's
range-rate (output filter) — but the fitted history velocity remains the
prediction backbone, so the tracker stays class-agnostic.

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

## Guidance output filter (fixed, `OUTF_*` in `src/cluster.c`)

What the **wire reports** for each target is a smoothed output filter, not the
raw per-frame cluster median. Output-only: association, gating and lifecycle
never see it (validated — confirmed-track lifecycles are bit-identical across
the whole fixture corpus with the filter on).

| Number | Value | Why |
|---|---|---|
| Angle smoother | alpha-beta on (az, az-rate) + (el, el-rate), Benedict-Bordner beta = a²/(2−a) | steady angles for the gimbal; rate state gives lag-free following of a constant-rate crosser. |
| Alpha tiers | 0.35 / 0.20 / 0.12 at <20 hits / ≥20 / ≥60 hits + peak SNR ≥21 dB | a young or faint track follows its measurements; an established bright one smooths hard. Far-segment az step-std ~3× lower at tier 1 (T7). |
| Range/vr filter | same alpha-beta + claimed-doppler median as a **direct** range-rate measurement (gain 0.35, gate 3 m/s) | doppler sign verified on this fw = range-rate (outbound walker claims +1.8 m/s); the gate rejects folded/soup doppler (a spur streak's claimed doppler is ~16–19 m/s off its real motion). |
| Coast / park | rates held through misses; park-held track bleeds rates ×0.5/frame | coasting continues the trajectory; a parked box must not drift on a stale rate. |
| Re-acquire slew | output angles converge ≤3°/s + track rate after a coast (`OUTF_SLEW_DPS`) | the gimbal never sees a re-latch teleport; normal tracking follows the filter exactly (no cap). |

## Walk guard (fixed, `WALK_*` in `src/cluster.c`)

Anti-breather **motion verification** on confirmed tracks: a real mover's
claimed doppler integrates to its actual range displacement; a multipath
breather claims doppler its position never delivers. Emits `mv_class` per
target on the wire (0 = UNVERIFIED_SLOW, 1 = VERIFIED_MOVER, 2 = SUSPECT) and
kills only on a sustained, decidable contradiction — a slow or tangential
target is *never* judged, only classed.

| Number | Value | Why |
|---|---|---|
| Window / decidability | 5 s; median \|claimed dop\| ≥ 1.2 m/s AND \|∫dop\| ≥ 3 m AND ≥ 1.5 s covered | below these the doppler story is too weak to judge — class 0, never kill. |
| Robust integration | per-frame claimed dop clamped to 3× window median; per-pair range step winsorized to 3×claim+0.5 m/s; gaps > 0.5 s not integrated; D and dR compared over the SAME covered pairs | one 30 m/s noise-point median on a 1.8 m/s walker must not race D ahead (T7 @ 188 m); a re-latch jump (+6 m in one gap, T2 @ 217 m) is artifact, not motion; a flickery far track must not fail by construction. |
| Pass tolerance | \|dR − D\| ≤ max(1.5 m + 0.01·r, 0.3·\|D\|) | endpoint noise grows with range (range-aware like every guard threshold). Pass ⇒ VERIFIED, latched (`mv_ever`, graveyard-inheritable); a latched track is never walk-killed (protects the T7 walker reversing at 306 m). |
| Cross-domain verify | coherent az net ≥ max(3 m, 2×2.5°·r), net/path ≥ 0.40, AND radial story sane | a tangential crosser is radially silent but really moving. Radial sanity required: the garage wanderer slides coherently in az while its range teleports +56 m vs claimed ~0 — cross evidence without radial sanity blocks the kill but earns no latch. |
| Kill | 13 consecutive decidable fails, evidence frames only, r ≥ 20 m, never-verified tracks only | ~0.5 s of contradicted measurements; below 20 m claimed doppler is near-field soup (T7 walker at 5–13 m claims +7.6 m/s) — near range belongs to the flood logic + consistency guard. |

Corpus validation 2026-07-13: T1/T3/T4/T5/T6/garage/c16/c3 bit-identical to
pre-walk-guard; T2/V2DAY emitted-track counts unchanged; human-corridor
coverage V2DAY 0.949 unchanged, T7 0.594→0.570 (chaos flips of two marginal
junk-mixed return-leg fragments — killed by the *consistency* guard, not the
walk guard); T7 turnaround segment identical, zero human kills.

## LLR track score (fixed, `LLR_*` in `src/cluster.c`)

A sequential **log-likelihood track score** runs beside the M-of-N
confirmation: hit adds `log(P_D · N(innovation) / λ_c)`, miss adds
`log(1 − P_D)`. `N` is the 2-D gaussian innovation density in (range, cross)
space with SNR-quality-weighted sigmas; `λ_c` is an **online clutter rate** —
an EWMA of unclaimed moving-point density per 50 m range annulus (10 annuli,
gain 0.02 after a 50-frame warmup, floored at 2e-4 /m²).

Confirmation = the existing fast (M-of-N) and slow (8-of-12) paths — both
**unchanged**, they remain the floor — OR `L ≥ 5.5` with the jitter gate and a
*reduced* moving-rate floor (`ST_MV_LO` 0.35, ~3 frames to build, vs
`MV_RATE_MIN`'s ~6; the full floor, not M-of-N, was the real latency binder).
Two robustness caps: per-hit increment clamped to [−2, +1.5] (an empty
annulus must not let one bright multipath coincidence confirm in two hits —
the AGV1 garage case), total score clamped to [−10, +30].

Calibration (2026-07-13, threshold scan 3.5–6.5 at zero emitted tracks on
c16/c3/AGV1/T6): **5.5**. Results: T7 far-human corridor confirm latency
17.0 → 12.3 frames (−28 %), corridor coverage 0.570 → 0.590, id switches
14 → 12; V2DAY human unchanged (day-close target already confirms at the
fast-path floor); zero emitted tracks on all four noise fixtures (AGV1/T6
bit-identical); confirmed-junk (never-emitted) population +10 %/+12 % on
c16/c3 — the score competes against the honest local clutter rate, so a
16 dB carpet stays hard to confirm from position alone by design.

## Doppler-native association (fixed, `DN_*` in `src/cluster.c`)

MOVING points are claimed in a **3-D gate** (range, azimuth, doppler): a point
must also tell the track's velocity story. `dr = |r_i − (r_t + vr_eff·dt)|/4 m`,
`da = |az_i − az_pred|/4°`, `dd = min_k |dop_i − dop_med + k·2·v_fold|/1.5 m/s`
(fold-aware, k ∈ {−1,0,1}, `V_FOLD_MPS` 41.45 from the A/G chirp timing —
matches the measured corpus max |doppler| 41.416); claim iff all three < 1 and
the sum < 2.5. `vr_eff` (range prediction) blends the claimed-doppler EWMA
with the fitted range-rate (young/incoherent tracks: claimed only); the `dd`
identity test is **claim-to-claim** (`dop_med`), never the blend — fast
vehicles on this DDMA fw carry a systematic claimed-doppler bias (T5 receding
car claims +11.5 m/s while moving ~18 m/s radially) and must match their own
claim. Static-channel points keep the old spatial gate; miss-growth still
widens dr/da (never dd).

Corpus (2026-07-13, vs the LLR-commit baseline): T7 human corridor coverage
0.590 → 0.777, id switches 12 → 3, off-corridor emitted share 0.175 → 0.120;
T2 night-walk segments merge (82 s continuous 165–283 m); T4 far human = one
track 87–203 m; T5 junction holds 2.5× more emitted frames; V2DAY human = one
139 s track to ~200 m, coverage 0.949 → 0.960; c16 confirmed junk 160 → 0,
c3 238 → 27; zero emitted tracks on all noise fixtures. Known cost: unclaimed
carpet points now seed short-lived tentative blips (max concurrent tracks 61
of 128 on the busiest fixture — no table pressure).

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
