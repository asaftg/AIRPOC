# V2 RADAR SHIP RUNBOOK (fw review PASSED — SHIP-WITH-FIXES, fixes folded)

FW image: agv2 sha256 173f622a...7245 (seeker repo firmware/flash_agv2.cfg;
review fix = ISR landmine removed + rebuilt). FW REVIEW MANDATES:
- FLASH WITH THE COMB GATE OFF (no emptyBandGateCfg line) = stock dataflow
  + crash fix only. Run all regression gates before touching the gate.
- The gate's 12 dB margin is a PLACEHOLDER: before production enablement,
  dump the Z-array for a corner reflector to fit the true LSB/dB slope,
  then margin-sweep (3/6/12/24 dB) against the 500 m corpus. The gate is a
  range-vs-ghost knob; guard the far-target detect rate while sweeping.
- Under real overload a deferred frame may carry mixed points (counted in
  queryDemoStatus "UART deferred frames") - host treats such frames as
  suspect. The 450-pt clamp makes this rare.
- emptyBandGateCfg must appear AFTER the doppler cfarCfg line (cfarCfg
  memsets the struct; out-of-order = silently off, fail-safe).

Gate: Build B firmware + guard SW + this plan each pass their dedicated
adversarial review agent. Only then is Asaf asked for the flash.

## Package contents
- FW "agv2": Build B image = comb-killer (empty-band leakage gate, DDM-only,
  default margin 12 dB) + crash-proofing (ISR assert -> deferral + counter;
  450-pt frame cap at the transport budget; ISR-safe fault record).
  Rollbacks kept: flash_agv1.cfg (current), flash_demoDDM.cfg (V1.0).
- SW: tracker consistency guard in radar/src/cluster.c (golden-validated,
  parity-proven vs Python reference), already-shipped cfg-push hardening
  (a7f9521, deployed with the same rebuild).
- CFG: unchanged at ship (16.0 + 0.75 + C0). Knob reopen AFTER acceptance.

## Ship sequence
1. Review verdicts green (3 agents) -> present numbers to Asaf.
2. [Asaf] radar USB to PC, J17+J20, 12V cycle. Flash agv2 (COM check first).
   [Asaf] J20 only, 12V cycle, USB back, 12V cycle again, START stack.
3. Acceptance ladder (each gates the next):
   a. Fingerprints at current cfg: >=26 Hz, 0 drops, RUN_CALIB err=0xffe-all-pass,
      no MON_TIMING_FAIL; ppf/static_snr vs same-scene pre-flash reference.
   b. COMB CHECK: junk-movers/frame ~250 -> expect near-zero (the headline).
      Real-mover retention on a quick walk.
   c. CRASH-PROOF STUNT: cfg 15.5 dB push -> garage + close strong mover
      (metal sheet / car). Chip MUST survive; uartDeferredFrames increments;
      recovery to full rate when stimulus stops. Then back to 16.0.
   d. 30-min soak in background.
4. Guard deploy: Jetson pull + daemon rebuild + STOP/START (no flash).
   Verify: gates-open knobs (snr 16, el +-20) -> display clean (panel-#3
   quality); garage ghosts zero; T7-style walk stitches long tracks.
5. Freeze new fingerprints as the V2.0 baseline (baseline.py freeze on fresh
   captures) + tag AIRPOC-RADAR-V2.0 (fw sha + cfg + sw commit) with revert
   recipe, same pattern as V1.0.
6. Schedule the bar-ladder walk night (16 -> 15 -> 14 -> 13) = the max-range
   answer, scored with walkout_score.py + trajectory plots.

## Rollback tree
- FW: flash_agv1.cfg (5 min) or flash_demoDDM.cfg (V1.0).
- SW: git checkout AIRPOC-RADAR-V1.0 radar/src + rebuild, or revert commit.
- CFG: per-step activation commits are single-line reverts.
- Tracker knobs: /ctl one-liners (snr/elmax) at any moment, no restart.
