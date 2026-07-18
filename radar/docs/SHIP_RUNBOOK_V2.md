# V2 RADAR SHIP RUNBOOK (rev 3 — all three ship-gates passed, defects folded)

> **STATUS 2026-07-17.** Steps 0–6 done — agv2 flashed and field-verified
> (survived the car-drive-by overload stimulus that killed agv1; 434 pts/frame
> peak, 0 deaths), SW guard deployed, cfg at 16.0 dB.
>
> The gate's failure to activate was root-caused (the `agv2` dB→raw scale was
> 2^14 too small) and fixed in **`agv3`, flashed 2026-07-17**. The gate now
> runs in **observe mode**, reporting per-detection margins and rejecting
> nothing. **Step 7 is now the measurement, not a bug hunt** — and the margins
> land in every recording via the `airpoc.radar_cli` tap, so step 7a's
> LSB/dB calibration is already answered on-chip (`lsbPerDb = 87081.6`).
> **Step 8 still pending** (gate first). Status map: [`ROADMAP.md`](ROADMAP.md).

FW image: agv2 sha256 173f622a...7245 (seeker repo firmware/flash_agv2.cfg).
Contents: overload crash fix (unconditional) + DDMA empty-band comb gate
(DEFAULT OFF — ships dark; enablement is step 7, after calibration).
Reviews: FW = SHIP-WITH-FIXES (fix folded, image rebuilt; the reviewed-vs-
shipped delta is one ISR-landmine removal — binary-unverifiable, accepted,
re-validated by this ladder). SW = see guard verdict. PLAN = GO-WITH-CHANGES
(defects D1-D10 folded into this revision).

## STEP 0 — BEFORE ANYTHING: push the firmware sources (D1)
On this PC with YOUR GitHub auth:
  cd C:\seeker-ground-station
  git push origin main --tags
This carries: Build A sources (1b71785, the fw ON the chip), Build B complete
package (4034864 + 6540c65 + f7e1f9c), and tag AIRPOC-RADAR-V1.0-FW. Until
this push, this PC holds the ONLY copy of the running + pending firmware.

## STEP 1 — pre-flash reference (D4)
Jetson ON, 12V-cycle radar, START stack.
  a. Read the boot identity: daemon log shows cfg push; CLI scrape or
     radar_hello equivalent -> record banner (proves agv1 before USB moves).
  b. Capture ~10 min of the bench scene (SSE tap or recorder) ->
     preflash_ref.bin via radar/tools/regression/sse2bin.py or conv_airec.py.
     This is the ONLY valid comparison reference for post-flash numbers
     (baseline.json is 17dB-era; the live cfg is 16.0 — apples-oranges).

## STEP 2 — deploy the SW package FIRST (D6 reorder)
Jetson: git reset --hard origin/main; rebuild radar daemon (make in
radar/src); STOP/START. This deploys: the consistency guard (65cb276) +
cfg-push hardening (a7f9521, every cfg line must ack "Done").
  Verify: /stats shows el_max/snr knobs; garage display CLEAN (guard active);
  a 2-min capture -> tracks: expect ZERO emitted ghosts in static garage.
  KNOB POSTURE: see the DECISION box below — set it now via /ctl if needed.

## STEP 3 — flash agv2
  [Operator] radar USB to PC, J17+J20, 12V cycle.
  python C:\ti\mcu_plus_sdk_awr2x44p_10_02_00_04\tools\boot\uart_uniflash.py
    --serial-port COM<N> --cfg C:\seeker-ground-station\firmware\flash_agv2.cfg
  (COM<N>: python -m serial.tools.list_ports -v -> XDS110 Application/User UART)
  [Operator] J20 only, 12V cycle, USB back to Jetson, 12V cycle, START stack.

## STEP 4 — post-flash acceptance (gate is OFF — stock dataflow + crash fix)
  a. Boot banner + cfg push all-Done (hardened push now enforces this).
  b. Rate >= 26.0 Hz, 0 drops. RUN_CALIB prints: err field = 0x0-equivalent
     all-pass semantics -> gate on the DONE-MASK bits 1-11 set (0xffe) and
     mask stability, NOT on cadence (D10a: the printed "err=0x80000ffe" IS
     the all-pass value per TI docs - field name is misleading).
  c. COMB CHECK (D2 corrected): junk-movers/frame must be UNCHANGED vs
     preflash_ref (gate is OFF - this proves bit-identical dataflow).
     Near-zero comb is step 7's success metric, NOT this step's.
  d. ppf / static SNR / real movers vs preflash_ref (same scene, same cfg).
  e. 30-min soak in background; mod-50 bucket check; no MON_TIMING_FAIL.

## STEP 5 — crash-proof stunt (the reason this flash exists)
Cfg with doppler CFAR 15.5 exists as radar/cfg/awr2944P_ag_v2_c0_c1a.cfg
BUT at 15.5 not 16 — activation = commit copying it over awr2944P_ag.cfg
with the threshold edited to 15.5, push, Jetson pull, 12V-cycle, START (D7).
  Stimulus: walk close + wave metal / drive the car by.
  PASS: chip SURVIVES (agv1 died in seconds here); rate degrades gracefully;
  "UART deferred frames" counter increments — read it via the CLI scrape
  (daemon frees the config UART after push; stty 115200 + printf
  "queryDemoStatus\n" — procedure proven 2026-07-10).
  Display may still show junk at 15.5 — judge by counter + rate, guard keeps
  the emitted tracks clean.
  Then revert cfg to 16.0 (single-line revert commit), cycle, verify.
  NOTE (D8): "host flags deferred-coinciding frames" is NOT implemented —
  follow-up item; the 450-pt clamp makes deferrals rare; risk accepted.

## STEP 6 — re-freeze baselines (D9 scoped)
Fresh bench captures only: garage/static + a T7-class walk if convenient.
baseline.py freeze for those classes; highway/junction/370m re-freezes wait
for the next field session. Tag AIRPOC-RADAR-V2.0 (fw sha + sw commit + cfg)
with the revert recipe, pattern of V1.0.

## STEP 7 — comb-gate enablement (scheduled, NOT tomorrow unless time allows)
  a. Corner reflector at surveyed range: dump the Z-array (L3) once, fit the
     true LSB/dB slope (the 5.31 LSB/dB is derived-but-unpinned; a 2^k error
     shifts the knob by 6k dB - soft failure, instantly visible).
  b. Margin sweep 3/6/12/24 dB on a comb-heavy scene: junk-movers 65-250/frame
     should collapse near the calibrated ~12 dB while real movers survive.
  c. A/B vs gate-off on the regression corpus + far-target retention
     (the margin is a range-vs-ghost knob - sweep with the 500m corpus).
  d. Enable in the shipping cfg (emptyBandGateCfg AFTER the doppler cfarCfg
     line - out-of-order = silently off) + re-freeze baselines.

## STEP 8 — bar-ladder walk night (the max-range answer)
16 -> 15 -> 14 -> 13 dB with the SAME walk each time, gate ON (step 7 first
if at all possible - at 14/13 the junk flood + 450-pt clamp can evict weak
far targets and make results clamp-limited, D3). Watch ppf vs 450 +
deferred-frames each step. Scored with walkout_score.py + trajectory plots.

## KNOB POSTURE — RESOLVED by the final SW ship-gate (round-2 re-review)
Guard round 2 (commit 6b24d7e) fixed all three round-1 blockers; reviewer
independently re-verified (incl. a synthetic 16-17dB drone @250m: EMITS via
the doppler-consistency relief; ghost streaks mechanically rejected at 4-5x
margin). VERDICT: DEPLOY at compiled defaults (snr 16, el +-20). The narrow
posture (el +-5) buys ~60 T4 track-frames and costs T5, T1 detect, and ALL
airborne elevation coverage - no reason to use it.

Round-2 vs live-today: ghosts 138/6831/0 -> 0/0/0; T2 far-hold 48.6s vs
32.6 (BETTER), far-band exact recovery; T7 walks ~= narrow reference; T4
mover 513 vs 588 (-13%); T1 false<200m .077 (flat), detect .680 vs .711
(residual confined to ghost-identical flicker + latch cycles).

THREE LOGGED ACCEPTANCES (operator sign-off at deploy):
1. T5 tangential queue/junction traffic -87% under the guard - the known
   crosstraffic blind spot, recovered by Phase 3 angle-MTI, not by knobs
   (narrow posture is equally bad: -90%).
2. Far-standing drift beyond 15m of the last-held spot only partially held
   (T7-style stand; T2-style stand fully recovered).
3. Watch items for first field sessions: "zombie" unlatched-but-alive
   drifters shadowing real seeds (garage multipath); grave-inheritance
   phantom-hold; +0.3s confirm latency (EO remains the fast channel).

## Rollback tree (unchanged)
FW: flash_agv1.cfg (Build A) / flash_demoDDM.cfg (V1.0). SW: revert commit /
checkout AIRPOC-RADAR-V1.0. CFG: single-line activation reverts. Knobs: /ctl.
