# AIRPOC Radar — Best-A/G Firmware Plan (v3, morning 2026-07-10)

Mission: A/G = vehicles (~500m), humans (~300m), DRONE BODIES (small RCS), any
direction, >=20Hz. Baseline never regresses. Production-grade only.
AWR2944P EVM, TLV point cloud over UART, NO DCA1000.
REVIEWS COMPLETE: physics (8/8 confirmed, 2 bookkeeping fixes folded) + baseline-risk (GO-WITH-CHANGES, folded) + completeness (13 gaps folded). BUILD A CLEARED TO FLASH. 
 

## A. Chip truth (CONFIRMED — full ledger in transcript-miner report)
- Chip runs awr2x44P_mmw_demoDDM_interleave.appimage sha 7292938f, flashed
  2026-05-22 19:48 UTC (flash #17 of 17), nothing after. Binary + mss source
  snapshot committed in seeker-ground-station; SDK build tree byte-identical.
- A/G path in it = byte-identical to stock TI DDM (ddmEnabled gates only skip
  DDM setup when the cfg lacks ddmPhaseShiftAntOrder; ours has it).
- TLV7 per-point SNR is on the wire (5 TLVs/frame, guiMonitor detectedObjects=1).
- Comb measured static with uptime; az of fixed reflectors stable ±0.05°; el
  noisy ±0.5–1° but stationary. antennaCalibParams = TI SAMPLE values (never
  measured on this board). ~300 pts/frame = CFAR yield (fw cap is 793).

## B. Root causes
1. Spur comb: DDMA phase quantization — 6-bit shifter (5.625°/LSB) with divisor
   6 → ±1.875° period-3 error → deterministic spurs, ~-31.5 dBc. Static by
   construction. DPU discards empty-band energy without a leakage test.
2. Far-range: flat 17 dB doppler-CFAR with the DPU's built-in range-dependent
   threshold (variableThresholdMode: +7 dB near, taper to base at max range)
   switched OFF. [pending physics-review confirmation of arg position/semantics]
3. Angle: sample calibration values + weak el aperture. NOT drift (measured).
4. ~4 ms/frame debug CLI_write in highest-priority task (A/A leftover).
5. Periodic RF calibration disabled 04-30 (LVDS motive, moot) → unbounded
   TX-power/RX-gain/phase drift across temperature. Not the comb cause (comb
   static), but hygiene + holds max range across sessions.
6. BFP compression 0.5 shares exponent per 8 range bins (20.8 m): weak target
   within ~21 m of a >25-30 dB stronger reflector is quantized away. Ratio 0.75
   (cube 1.77 MB vs known-good 1.18 / known-bad 2.36) likely fits and closes it.
   Compression must stay ON (proven 05-29: chip CFAR dies without it).

## C. FLASH BUILD A — "un-bend" (this morning, gated on reviews)
Built + committed: awr2x44P_mmw_demoDDM_agv1.appimage sha a52970d2, seeker repo
commit 1b71785 (LOCAL — no GitHub auth on this PC; Asaf pushes).
- A1. enablePeriodicity=true, periodicTimeInFrames=50 (~1.9 s @26 Hz).
- A2. Blanket per-frame BSSEV CLI_write removed (fault + calib prints kept).
Flash command (exact):
  python C:\ti\mcu_plus_sdk_awr2x44p_10_02_00_04\tools\boot\uart_uniflash.py
    --serial-port COM<N> --cfg C:\seeker-ground-station\firmware\flash_agv1.cfg
NEVER edit flash_demoDDM.cfg — it IS the rollback (05-22 image):
  same command with --cfg C:\seeker-ground-station\firmware\flash_demoDDM.cfg
COM<N> discovery: python -m serial.tools.list_ports -v → XDS110 Application/User
UART (the flasher only talks on that one of the XDS110 pair).
Mid-flash failure (the "47%" mode): SBL untouched at 0x0, QSPI restartable —
12V power-cycle (still SOP 101), retry. 3 fails → different USB port, no hub,
close any terminal holding the COM. Appimage-region failure cannot brick.

### Build A morning PASS criteria (numbers, not vibes)
- Boot banner present (radar_hello.py / version) → record identity in ledger.
- "BSSEV RUN_CALIB done=..." prints on CLI = A1 alive. Cadence may be ~1 s
  (internal APLL/SYNTH reports per DFP) not only ~1.9 s — gate on err=0x0 and
  done-mask stability, NOT on cadence.
- ZERO MON_TIMING_FAIL prints over a 30-min soak.
- Rate >= 26.0 Hz, 0 drops (baseline 26.25–26.39).
- Comb fingerprint (scene-independent): false_mv <= 105 (base 92–96),
  false_snr <= 19 dB (base 17.0) — must hold on ANY capture.
- ppf / static_snr / real_mv within tolerance vs preflash_ref.bin (SAME scene,
  captured pre-flash — NOT vs baseline.json which is other days' scenes).
- SOAK (30 min, cold boot = first power-on of day): drift_test.py → fixed
  reflectors stationary (±0.05° az), comb rate flat across calib events; diff
  comb fingerprint first-5-min (cold) vs last-5-min (warm).
- MOD-50 BUCKET CHECK: bucket per-frame ppf / real-movers / median SNR by
  frame_index % 50; any step aligned to the calib phase = boundary artifact
  (median fingerprints alone would hide it). Revert or raise period if seen.
- CORNER-REFLECTOR CENTROID: az/el/range centroid of a fixed reflector pre vs
  post; flag >0.2° az or >1 range-bin shift (covers the angle/range blind spot).
- RUN_CALIB SCRAPE: 10 min of CLI log — done-mask stable, err=0x0 always, temp
  drifting smoothly.
- ANY line red → reflash flash_demoDDM.cfg; day continues on old fw with the
  CFG wave (C-items don't require Build A).

## C0–C5. CFG WAVE (no flash; chip reconfigures ONLY from power-on INIT →
12V-cycle radar before every push; AIRPOC daemon pushes on start when chip
not already streaming — verify log does NOT say "chip already streaming")
- C0 (free headroom): guiMonitor logMagRange 1→0 — drops the unconsumed
  rangeProfile TLV (~392 B/frame = 192 bins x 2B + 8B header, ~10 KB/s).
  Verify AIRPOC daemon happy w/ 4 TLVs.
- C1 — SPLIT into two separate A/Bs (risk-review catch: bundling means base 14
  + 7 dB near guard = near threshold 17→21 dB, a +4 dB REGRESSION at 0–125 m
  where the proven walking-human capability lives):
  - C1a: flat threshold 17→15.5 (then 15 if clean). No variable mode. KEEP if
    farthest confirmed real target +10% AND >=26 Hz 0 drops AND negative-control
    flat AND near-human walkthrough (50–125 m) unchanged.
  - C1b (separately, after C1a verdict): variableThresholdMode=1 with base
    re-tuned so NEAR effective threshold never exceeds today's 17 (i.e. base
    <= 10 → near 17, far 10) — pending physics-review confirmation of the
    taper semantics before any push.
  - Both: watch ppf vs 793 cap (>~600 sustained = UART pressure; ~500 pts ≈
    84% wire), far-bin share vs per-gate 40-pt cap (range histogram pre/post),
    bytes/frame + dsp_proc (TLV6) logged per step.
- C2 (falses cut, drone-safe): aoaFovCfg -1 -90 90 **-15 55** (ASYMMETRIC:
  multipath/ground-bounce live below horizon → cut -90..-15; drones occupy
  0..+55 in closing geometry — 100 m alt @ 200 m = 26.6°, grows terminal;
  never cap upper el below +55). KEEP if false_mv -15% AND real_mv >= 0.9x ref
  AND drone check clean.
- C3: compressionCfg 0.5→0.75. KEEP if sensorStart accepts AND dsp_proc
  (TLV6) < 34 ms AND weak-near-strong test point visible. Error = revert, done.
- C4: osKvalue 7→16 (after C1 verdict — they interact). KEEP if clutter-edge
  falses drop AND real_mv >= 0.9x.
- C5 (optional): peakGroupingEn 1→0 — more pts/human for M-of-N; watch UART.
- C6 (bench, no code) — INCIDENT-CLASS, own gated mini-project (same class as
  the reverted 0xffe calibration-word regression): corner-reflector run of
  measureRangeBiasAndRxChanPhase → per-unit antennaCalibParams. Rules: NEW cfg
  file (never edit shipping), surveyed reflector in an open scene, angle +
  detection A/B vs old values before adoption; bad cal is worse than sample cal.
- C2 upper-el note: risk-review suggests +40 cap (drone 100 m rng/60 m AGL =
  31°), completeness critic says never below +55 (closing FPV terminal
  geometry). DEFAULT = -15..+55 (drone-protective); Asaf may tighten to +40
  for more false-cut. Lower cut -15 agreed by both. ASSUMES level boresight
  (elevation is radar-frame): verify mount tilt < ~5° — a +10° up-tilt puts a
  20 m ground target at ~-14°, brushing the cut.
- ANY C-item: comb false_snr > 19 dB or drops > 0 = automatic revert.
- DRONE CHECK (mandatory per C-verdict): hover bench FPV at el >= 30°, 60 s +
  one closing pass; point count + max range not regressed. If no drone today:
  C2 ships only in asymmetric form; capture drone fixture T6 first opportunity,
  freeze into baseline.json before further el/threshold tuning.

## D. Where results go (three targets, different rules)
0. AIRPOC cfg changes (C0–C5): disposable scratchpad clone of the AIRPOC cloud
   repo → edit radar/cfg → commit→push main → Jetson reset --hard pull →
   12V-cycle radar → daemon pushes. One commit per C-item, verdict in message.
1. Firmware ledger / flash cfgs / verify_cfg fix: seeker-ground-station —
   committed LOCALLY (1b71785); NO GitHub auth here; Asaf pushes when ready.
2. Regression net (baseline.py/json, regression_gates.py, fixtures T1–T5,
   drift_test.py, conv_airec.py, comb analysis scripts, this plan):
   DURABLE at C:\jetson-stage\ag_regression\ (copied tonight).
   Gate tooling: baseline.py check <bin> <T#> = per-fixture morning tool;
   regression_gates.py = rj/rh-class scenes ONLY (its 280–340 ppf band fails
   sterile scenes by design).

## E. Morning runbook (Asaf physical parts in [brackets])
0. Asaf reviews this plan + both review verdicts. Push seeker repo when convenient.
1. [Jetson ON] 12V-cycle the radar FIRST, then read the BOOT BANNER before
   anything else (radar_hello.py / daemon cfg-push log) → confirms the running
   image identity BEFORE the USB ever moves (kills rollback-identity risk).
   Then START stack (:8088), capture ~10 min of the bench scene on the CURRENT
   firmware → recorder export → conv_airec.py → preflash_ref.bin (paired
   same-scene reference; tightens sensitivity resolution to ~0.3 dB).
2. STOP stack. [Move radar XDS110 USB to this PC. Jumpers J17+J20 closed
   (SOP 101). 12V power-cycle.]
3. Find COM port; flash flash_agv1.cfg (~2 min). On SUCCESS: [jumpers back to
   RUN = J20 only (SOP 001); 12V power-cycle; USB back to Jetson].
4. [Jetson] 12V-cycle radar again so chip is in INIT & NOT streaming → START
   stack → daemon log MUST show cfg push (not "already streaming") → feeds up.
   (connected:0 = launcher shm-tap gremlin → STOP→START.)
5. Run verify: rate ≈26.3 Hz, 5 TLVs, RUN_CALIB cadence ~1.9 s, no MON_TIMING_FAIL.
6. Same-scene capture → baseline.py + compare vs preflash_ref.bin + comb
   fingerprint. 30-min soak in background while doing C0.
7. PASS → CFG wave C0→C2→C1→C3→C4 (one variable at a time, decision rules
   above, negative-control before believing improvements).
8. FAIL anywhere → rollback flash_demoDDM.cfg (5 min), CFG wave still runs.

## F. BUILD B — "comb killer" (next; preserved so it survives this session)
- B1. Empty-band leakage gate: export DDMA-Metric hypothesis energies (mirror
  the Max-Subband EDMA out, dopplerprocDDMAcommon.h:198-240) → in
  DPU_DopplerProcHWA_extractObjectList (dopplerprochwaDDMA.c:629-768 and
  :835-1029) reject candidates with winning-band-minus-empty-band margin
  < ~12 dB (rationale: comb sits ~-31.5 dBc → real targets clear 12 dB easily,
  leakage doesn't). Validation: TLV replay CANNOT validate a DPU change (wire
  content changes) — same-scene sequential A/B + comb fingerprint
  (false_mv/false_snr collapse) is the kill-confirmation. Re-freeze baselines
  AFTER the winning CFG set locks, else B is judged against stale references.
- B2. Hygiene: #undef LVDS_STREAM (mmw_mss.h:83), delete CBUFF cycle hook +
  [SEEKER] prints (frees 14 EDMA ch, 12 ms boot delay).
- B3 (only if residual comb): divisor-8 DDMA (4 empty bands = exact 45° steps,
  zero quantization error; mss_main.c:2892-2894 case 4 → 4, chirpCfg 0 7,
  frameCfg 0 7 96) — numBandsTotal=8 UNVALIDATED in TI DPU; high risk; full
  bench + replay gates.
- Comb characterization justifying the 12 dB margin: comb_charac.py, spur_test.py
  (durable copy in C:\jetson-stage\ag_regression\).

## G. Expected gains per target class
- Vehicles 380→500 m: C1 (~3 dB at far bins) + C3 (scene holes) + A2 headroom.
- Humans 230→300 m: same + C5 point multiplicity; B1 unburies weak movers.
- Drone bodies: C1+C3+B1 (smallest RCS benefits most); no hard number until
  bench; protected from regression by the drone check + asymmetric C2.
- Tangential blindness: NOT solved here — B1 cleans Doppler for the small
  radial component of angled crossers; full angle-MTI/heatmap detector remains
  the standing Phase-3 item (project_airpoc_radar_alldirection_plan).
- Angle accuracy: C6 per-unit cal + host track-level averaging.

## H. Open items
- Physics review verdicts (resumed, running) — esp. variableThresholdMode
  semantics + Build A edit correctness. FOLD IN BEFORE FLASH.
- Baseline-risk review verdicts (resumed, running) — GO/NO-GO + morning checks.
- MON_TIMING_FAIL behavior under periodic calib at 38 ms frame — bench, step 5.
  NOTE (physics review): calibMonTimeUnit = 1 frame = 38 ms is BELOW the DFP
  valid range 40-250 ms (rl_sensor.h:3294, pre-existing). If MON_TIMING_FAIL
  fires, the remedy is calibMonTimeUnit=2 (code change), NOT larger
  periodicTimeInFrames. Calib worst case ~4.7 ms in 145 us slices vs ~20 ms
  inter-frame idle → unlikely to fire.
- Compression 0.75 L3 fit — sensorStart accept/reject decides (C3).
- Seeker repo push (needs Asaf's GitHub auth).
