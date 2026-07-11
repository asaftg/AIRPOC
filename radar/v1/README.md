# Radar V1 (frozen 2026-07-10, tag AIRPOC-RADAR-V1.0)

The complete known-good radar version as field-used through 2026-07-10.
- fw/: interleave appimage (sha 7292938f, on-chip 2026-05-22 -> 2026-07-10)
  + flash_v1.cfg + the mss source snapshot it was built from.
  Base: TI mmw_demoDDM SDK 4.7.2.1 + 5 stability patches + ddmEnabled gate.
  Periodic RF calibration DISABLED in this version.
- cfg/: awr2944P_ag.cfg at V1 (17.0 dB doppler CFAR, compression 0.5,
  rangeProfile TLV on, 26.3 Hz dead-time trim).
- sw/: tracker (cluster.c/h) at V1 - hard elevation window -9..+2.5 deg,
  no consistency guard.
Full system state: git tag AIRPOC-RADAR-V1.0. Verified: veh ~380m radial,
human ~230m (350m best night), 26.3Hz 0 drops, ~95 ghost movers/frame @17dB.
Datapath fw sources for V1 = stock TI SDK (no datapath edits existed).
