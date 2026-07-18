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
