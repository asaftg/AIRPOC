# AIRPOC RADAR V1.0 — frozen revert point (2026-07-10)

The complete known-good radar stack as field-used through 2026-07-10.
To revert ANY layer, use exactly these:

## FW (on-chip)
- Image: `awr2x44P_mmw_demoDDM_interleave.appimage`
  sha256 `7292938f90add2c33e3f770c5ea99fa049a44e7b25501bf6319152daf4225b66`
- Flashed 2026-05-22 19:48 UTC, QSPI 0xA0000 (+ sbl_qspi.release.tiimage @0x0)
- Source + binary live in the `seeker-ground-station` repo
  (`firmware/awr2x44P_mmw_demoDDM_interleave.appimage`, source snapshot
  `firmware/mmw_ddm_patched_src/` at commit `102fd1f`; local tag
  `AIRPOC-RADAR-V1.0-FW` marks that commit)
- Reflash: `python C:\ti\mcu_plus_sdk_awr2x44p_10_02_00_04\tools\boot\uart_uniflash.py
  --serial-port COM<N> --cfg C:\seeker-ground-station\firmware\flash_demoDDM.cfg`
  (jumpers J17+J20 = flash mode; J20 only = run mode; 12V cycle each change)
- Base: TI mmw_demoDDM, mmwave mcuplus SDK 4.7.2.1 + 5 SEEKER stability patches
  + ddmEnabled gate. Periodic RF calibration DISABLED in this version.

## CFG (pushed to chip at daemon start)
- `radar/cfg/awr2944P_ag.cfg` at this tag (A/G long-range: N=384, slope 4.5,
  26 Hz dead-time trim idle3/ramp20.5/period38, doppler OS-CFAR 17 dB,
  compression BFP 0.5, aoaFov az ±90 el ±90, LVDS off)

## SW (Jetson)
- This repo at tag `AIRPOC-RADAR-V1.0` — radar_preview C daemon (:8092) with
  temporal tracker (cluster.c), recorder, launcher, console as of this commit.

## Verified behavior at V1.0 (fingerprints)
- 26.3 Hz, 0 drops, ~300 pts/frame, dsp_proc 17.05 ms
- Radial detection (operator-tested): vehicles ~380 m, humans ~230 m (night 350 m best)
- Known limitations: tangential blindness (Doppler~0 crossers), DDMA spur comb
  (~95 false movers/frame @17 dB), sample antennaCalibParams (angle accuracy),
  radial-only velocity trust
- Regression fixtures + gates: `C:\jetson-stage\ag_regression\` (T1–T5 + baseline.py)
