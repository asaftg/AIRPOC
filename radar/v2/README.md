# Radar V2 (built + triple-reviewed 2026-07-11; flash pending)

- fw/: agv2 appimage (sha 173f622a) = overload crash fix (unconditional:
  deferral + counter instead of ISR self-destruct; 450-pt frame clamp at the
  UART transport budget; ISR-safe crash record in queryDemoStatus) + DDMA
  empty-band comb gate (DEFAULT OFF; CLI emptyBandGateCfg <sf> <en> <dB>,
  must follow the doppler cfarCfg line; enable only after the LSB/dB
  calibration - docs/SHIP_RUNBOOK_V2.md step 7). agv1 image + rollback cfg
  included (the intermediate 2026-07-10 build, currently on-chip pre-flash).
  src_mss/ + src_datapath/ = the exact sources (SEEKER-marked; datapath
  README maps SDK destinations + rebuild procedure).
- cfg/: awr2944P_ag.cfg at V2 (16.0 dB, compression 0.75, rangeProfile off).
- sw/: tracker with the consistency guard rounds 1+2 (commits 65cb276 +
  6b24d7e): ghost kill (0/0/0 on junk fixtures), open elevation +-20 default,
  emission evidence + faint-far doppler-consistency relief.
Ship gates: FW + SW + PLAN reviews all passed (see docs/SHIP_RUNBOOK_V2.md).
Known acceptances: tangential queue traffic weak until Phase 3; far-stand
drift >15m partial; +0.3s confirm latency.
