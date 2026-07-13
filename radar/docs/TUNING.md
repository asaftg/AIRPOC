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
