# Radar V3 (shipped 2026-07-21 — flashed, built, live on the Jetson)

Tag `AIRPOC-RADAR-V3.0`. See [`../VERSIONS.md`](../VERSIONS.md) for the
fw+cfg+sw matrix and the revert procedure.

- **fw/**: `agv3` appimage (sha `e26c7460`) — **ON-CHIP since 2026-07-17**.
  agv3 = agv2 (overload crash fix + DDMA empty-band comb gate) + the comb-gate
  dB->raw margin scale FIX (agv2's conversion was 2^14 too small, so the gate
  rejected nothing at any setting; ~87082 raw LSB/dB, verified vs TI SWRU526),
  plus observe-only mode and per-detection margin telemetry in
  `queryDemoStatus`. The gate currently runs in **observe mode (2)**: it
  measures margins and rejects nothing, pending calibration before arming.
  `src_mss/` + `src_datapath/` are the exact sources.
- **cfg/**: `awr2944P_ag.cfg` as run (unchanged from V2).
- **sw/**: `cluster.c` + **`slowdet.c`** + **`fuse.c`** (+ headers, `main.c`,
  `Makefile`) — the two-detector stack. cluster confirms per frame; slowdet
  chains faint intermittent echoes across frames; fuse merges them into one
  class-less box per object and conditions the published elevation.

Measured on the fixture corpus: negatives 0/0/49, positives identical to the
reference, elevation frame-to-frame jump 4.06 deg -> 0.17 deg, 446 us/frame =
1.2 % of the 26 Hz budget. Live on the bench the same day: a vehicle held at
~157 m closing 12 m/s with elevation steady within 0.02 deg.

What it does: [`../docs/SLOWDET.md`](../docs/SLOWDET.md).
What we tried that failed: [`../docs/TRIAL_AND_ERROR.md`](../docs/TRIAL_AND_ERROR.md).
