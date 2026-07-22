# Radar V2 (shipped 2026-07-11 — flashed + field-verified)

- fw/: agv2 appimage (sha 173f622a, ON-CHIP since 2026-07-11) = overload
  crash fix (unconditional: deferral + counter instead of ISR self-destruct;
  450-pt frame clamp at the UART transport budget; ISR-safe crash record in
  queryDemoStatus) + DDMA empty-band comb gate (CLI emptyBandGateCfg
  <sf> <en> <dB>, must follow the doppler cfarCfg line). agv1 image +
  rollback cfg included (the intermediate 2026-07-10 build agv2 replaced).
  src_mss/ + src_datapath/ = the exact sources (SEEKER-marked; datapath
  README maps SDK destinations + rebuild procedure).
- cfg/: awr2944P_ag.cfg at V2 (16.0 dB doppler CFAR, compression 0.75,
  rangeProfile off).
- sw/: tracker with the consistency guard rounds 1+2 (commits 65cb276 +
  6b24d7e): ghost kill (0/0/0 on junk fixtures), open elevation +-20 default,
  emission evidence + faint-far doppler-consistency relief.

Field-verified: survived the car-drive-by overload stimulus that bricked the
pre-fix fw twice (434 pts/frame peak, 0 deaths); human ~300 m night-quiet /
~200 m day-busy, vehicle radial echoes ~424 m.

SUPERSEDED 2026-07-17 by `agv3` (../v3/fw/) — kept as the V2 rollback target.
The comb gate not activating on this image was the dB→raw threshold scale
being 2^14 too small; `agv3` fixes it. Do not enable `emptyBandGateCfg`
against this image: it parses the line and arms the filter with the broken
scale, silently deleting detections.

Known acceptances:
tangential queue traffic weak until Phase 3; far-stand drift >15 m partial;
+0.3 s confirm latency. Status map: ../docs/ROADMAP.md; ship record:
../docs/SHIP_RUNBOOK_V2.md.

---

## V2 as a revert point (recorded 2026-07-21)

Tag `AIRPOC-RADAR-V2.0` marks commit `6b97705` — the last state with **one
detector** (`cluster.c` only), which is what ran in the field through
2026-07-20. `sw/` here holds that exact snapshot (`cluster.c`, `cluster.h`,
`main.c`, `Makefile`).

Note the **chip ran `agv3`** by then, not the agv2 image in `fw/` — agv3 is a
pure firmware upgrade that did not change the software contract. The agv1/agv2
images are retained here as older firmware rollback points. See
[`../VERSIONS.md`](../VERSIONS.md) for the full fw+cfg+sw matrix.

To go back to one detector from V3: `git revert f800699` then rebuild — the
firmware and cfg do not change.
