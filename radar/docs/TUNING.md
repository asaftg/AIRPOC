# Radar detection parameters & tuning

The pipeline, then every knob: what it does, where it lives, whether it's live,
and whether its value is **principled** or an **inherited default that still
needs tuning against real returns**.

```
chip CFAR → point cloud (+per-point SNR) → point gates → DBSCAN
  (range-adaptive) → NN association (stable ids) → per-frame boxes
```

## Where each knob lives

- **Live** — set at runtime via `GET /ctl?...` (previewer sliders or the GUI);
  no restart. Echoed in `/stats`.
- **Fixed** — compile-time `#define` in `src/cluster.c`; change = rebuild.
- **cfg** — in `cfg/awr2944P_ag.cfg`, applied on the chip; change needs a
  **radar power-cycle** (this firmware takes one cfg per power-cycle).

## The knobs

| Knob | Value | Where | Basis | Effect / how to tune |
|---|---|---|---|---|
| **cluster ε** (near base) | 8 m | live | inherited (ground bench `eps_pos`) | "how close = same object" at the sensor. ↑ merges more, ↓ splits. |
| **ε range slope** | 0.06 | fixed (`EPS_RANGE_SLOPE`) | **my estimate** (~target angular spread ≈ 3.4°) | ε grows `+slope·range` (dots spread with range). **Unverified** — walk a person at 30/60/100 m, measure real cluster spread, set to match. |
| **min pts** | 2 | live | inherited | eligible neighbours needed to seed a cluster. ↑ = fewer/cleaner boxes, drops sparse far targets. |
| **min speed** | 0.4 m/s | live | inherited (`speed_min`) | dynamic-only gate; slower dots = static clutter, excluded. Walking humans are 0.5–2 m/s. |
| **min SNR** | 0 dB (off) | live | **new** | per-dot confidence gate *on top of* the chip's ~17 dB CFAR floor. 0 = trust CFAR only. ↑ to reject weak/noise, ↓ to reach for faint far dots. Unknown-SNR dots always pass. |
| **doppler gate** | 3 m/s | fixed (`EPS_DOP_MPS`) | inherited | two dots only join if radial speeds within this. Too small → one target (limbs / rigid-body spread) fragments; too large → different-speed objects merge. |
| **CFAR floor** | 17 dB (range) | cfg | inherited/proven | the chip's detection threshold = hardware sensitivity floor. Lowering (toward the ~16 dB min before flooding) reaches farther but adds clutter — pair with a host `min SNR`. |
| size clamp | 0.25–3.0 m | fixed | inherited | box half-extent limits. |
| assoc gate | 5 m (+growth) | fixed | inherited | NN association radius for stable ids. |
| M-of-N confirm | 2 of 3 | fixed | inherited | a new track must hit 2 of 3 frames before it's published (kills 1-frame noise boxes). |
| track miss budget | 5 frames | fixed | new | how long an unmatched track survives *internally* (never published) for id continuity. Not coasting. |

## How to tune (empirical, once we have the board + targets)

1. Open the previewer (`:8092`), walk a person and drive a vehicle at known
   ranges (10 / 30 / 60 / 100 m).
2. **min SNR:** slide up from 0 and watch weak clutter dots drop while the
   target (strong, ~40–55 dB) holds. Set the default just below where the target
   starts to thin out.
3. **ε + ε-slope:** confirm one box per target across the whole range sweep —
   if a far target fragments, raise the slope; if two near targets merge, lower ε.
4. **min speed / doppler gate:** confirm a slow-walking person still clusters
   and a person near a vehicle stays a separate box.
5. Record the values that work and fold the *fixed* ones (`EPS_RANGE_SLOPE`,
   `EPS_DOP_MPS`) back into `cluster.c`; the *live* ones become the shipped
   defaults in `cluster.h`.

> None of the inherited defaults (8, 3, 0.4, 17) has been re-validated for
> AIRPOC's sensor mount and scene yet. Treat this doc's values as a starting
> point, not ground truth. See [`ROADMAP.md`](ROADMAP.md).
