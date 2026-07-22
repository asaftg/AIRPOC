# Radar versions — what fw + cfg + sw go together

One row per shipped stack. **All three layers must match** — a bundle is only a
revert point if you take the whole row. Each version's files are under
`radar/vN/`, and the tag marks the exact repo commit.

| version | tag | fw (on chip) | cfg | sw (radar/src) | state |
|---|---|---|---|---|---|
| **V1.0** | `AIRPOC-RADAR-V1.0` | `demoDDM_interleave` `7292938f` | `awr2944P_ag.cfg` | pre-guard tracker | frozen 2026-07-10 revert point ([`VERSION_V1.0.md`](VERSION_V1.0.md)) |
| **V2** | `AIRPOC-RADAR-V2.0` | `agv3` `e26c7460` | `awr2944P_ag.cfg` | `cluster.c` only — **one detector** | the stack as field-run up to 2026-07-20 |
| **V3** | `AIRPOC-RADAR-V3.0` | `agv3` `e26c7460` | `awr2944P_ag.cfg` | `cluster.c` + `slowdet.c` + `fuse.c` — **two detectors, conditioned elevation** | shipped + live 2026-07-21 |

## What changed between V2 and V3

**Software only — the firmware and the cfg are identical.** V3 adds a second
detector and an output stage:

- `slowdet.c` — chains faint, intermittent echoes across frames to hold what the
  per-frame tracker cannot confirm (a 300 m car returning a single point in ~60 %
  of frames; the night walker past 240 m).
- `fuse.c` — merges the two detectors into **one class-less box per object**
  (cluster authoritative, slowdet only ever adding, slowdet ids ≥ 1000), and
  conditions the published **elevation** (range-scaled trailing median + rate
  limit + vertical box cap). Range and azimuth are untouched at full rate.
- `main.c` / `Makefile` — wiring only.

Measured: negatives 0/0/49 on the corpus, positives identical to the reference,
elevation frame-to-frame jump 4.06° → 0.17°, **446 µs/frame = 1.2 %** of the
26 Hz budget. Details in [`docs/SLOWDET.md`](docs/SLOWDET.md); everything that
did **not** work is in [`docs/TRIAL_AND_ERROR.md`](docs/TRIAL_AND_ERROR.md).

## Reverting

**V3 → V2** (drop the slow detector, keep the firmware):

```
git revert f800699          # the one commit that added slowdet + fuse + wiring
cd radar/src && make        # on the Jetson
```

or restore `radar/v2/sw/` over `radar/src/` and rebuild. Nothing on the chip
changes — same `agv3` image, same cfg.

**V2 → V1.0** is a firmware operation as well; follow
[`VERSION_V1.0.md`](VERSION_V1.0.md) exactly.

> Note: `radar/v2/fw/` also retains the `agv1` and `agv2` appimages plus their
> flash cfgs. Those are older *firmware* rollback points from the V2 era; the
> V2 **stack** as last field-run used `agv3` on the chip, which is why the table
> above lists `agv3` for both V2 and V3.
