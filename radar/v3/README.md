# Radar V3 firmware (agv3) - built + triple-reviewed 2026-07-11; FLASH-READY (not yet flashed)

agv3 = agv2 (crash fix + comb gate) + the comb-gate margin scale FIX and
on-hardware live-calibration tooling. agv2's dB->raw margin conversion was
2^14 (16384x) too small, so the gate rejected nothing at any setting. Fixed
(~87082 raw LSB/dB, verified vs TI SWRU526). ADDS:
  - observe-only mode (emptyBandGateCfg mode 2): measures per-frame margins,
    rejects NOTHING - for calibration.
  - per-frame margin telemetry (min/max/histogram) via queryDemoStatus.
  - raw-LSB threshold override (5th arg) bypassing the dB model.
appimage sha256 e26c7460...eae3. Flash: fw/flash_agv3.cfg. Rollback: v2 agv2.

REVIEWS (3/3 pass): scale-correct (TRM re-derivation); SHIP (no ABI skew, 3-core
clean rebuild, crash fix byte-intact, gate-off bit-identical); GO-calibrate.

CRITICAL - the gate is a range-vs-ghost knob and its margin ~= SNR-12dB, so a
faint-far target (250-300m, ~16-18dB SNR -> ~4-8dB margin) sits NEAR the ghost
regime (~0-4dB). NEVER arm reject (mode 1) before calibrating: observe on a
garage (ghost cluster) AND an open scene with a real far human; the excess
rejections while the far human is present must be < 0.2/frame. Garage alone
mis-sets it. See ../docs/SHIP_RUNBOOK_V2.md step 7.
