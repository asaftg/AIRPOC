# Radar regression corpus — fixtures & how to regenerate them

Fixtures are NOT committed (large binaries). Each derives from a recording
session that persists on the Jetson at /data/recordings/<sid>. Regenerate:
  python3 radar/tools/regression/conv_airec.py /data/recordings/<sid> <out>.bin
Format: repeated [double t_sec LE, int32 n, n x (5 float32: range_m, az_deg,
el_deg, doppler_mps, snr_db)].

| Fixture | Session | Content | Chip cfg | Role |
|---|---|---|---|---|
| T1 | 20260707T034753Z | sterile scene, many humans+vehicles | 17 dB | real-mover no-regression floor |
| T2 | 20260709T055004Z | night human radial walk to ~350 m | 17 dB | radial-human range reference |
| T3 | 20260709T065338Z | highway (tangential vehicles) | 17 dB | tangential blindness reference |
| T4 | 20260709T065130Z | far human 370 m day | 17 dB | far-human reference |
| T5 | 20260709T065503Z | junction crossers 370 m | 17 dB | tangential blindness core case |
| T6 | 20260709T010421Z | garage, wandering-track bug | 17 dB | guard kill-side (truth: 0 movers) |
| T7 | 20260711T051011Z | night human walk to ~300 m | 16 dB | guard keep-side + V2 walk reference |
| c16/c3 | (SSE captures 2026-07-10, regenerable by re-capture) | static garage | 16 dB | 16 dB junk-flood kill-side |

Baseline fingerprints: radar/tools/regression/baseline.json (frozen 2026-07-10,
V1.0-era chip cfg). Check a capture: `baseline.py check <bin> <T#>`.
regression_gates.py applies only to rj/rh-class scenes (see header).
