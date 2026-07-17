# Test recordings — the living catalog

Every recording we validate against, what is really in it, and what it proves.
Fixtures are NOT committed (large binaries); they derive from recordings that
persist on the Jetson at `/data/recordings/<sid>`. Regenerate:
`python3 radar/tools/regression/conv_airec.py /data/recordings/<sid> <out>.bin`
Format: repeated `[double t_sec LE, int32 n, n x (5 float32: range_m, az_deg,
el_deg, doppler_mps, snr_db)]`. Checksums are pinned in
`radar/tools/regression/manifest.json`; the bench refuses fixtures whose bytes
changed. c16/c3 are SSE captures — never regenerate, the pinned sha is the
recording.

## What we have

| Name | Session | Scene (the ground truth) | Proves | Chip | Day/Night | Truth sidecar |
|---|---|---|---|---|---|---|
| T1 | 20260707T034753Z | sterile scene, many humans + vehicles moving | real-mover floor — a change may not lose the easy targets | 17 dB | day | no |
| T2 | 20260709T055004Z | one human walking radially out to ~350 m | radial-human range reference | 17 dB | night | no |
| T3 | 20260709T065338Z | highway, vehicles crossing (tangential) | tangential-blindness reference (near-zero emissions today) | 17 dB | day | no |
| T4 | 20260709T065130Z | far human ~370 m, long pauses | far-human hold | 17 dB | day | no |
| T5 | 20260709T065503Z | junction crossers at ~370 m | tangential blindness, core case | 17 dB | day | no |
| T6 | 20260709T010421Z | garage, nothing real moves (wandering-ghost bug scene) | negative: exactly 0 emissions | 17 dB | night | yes (empty) |
| T7 | 20260711T051011Z | night walk out to 306 m, turnaround, RETURN leg to 9 m | guard keep-side + range truth both directions | 16 dB | night | yes |
| AGV1_garage | (agv1 capture) | garage, nothing real moves | negative: exactly 0 emissions | 16 dB | night | yes (empty) |
| c16 / c3 | SSE captures 2026-07-10 | static garage, junk-flood conditions | negative: exactly 0 emissions under point flood | 16 dB | night | yes (empty) |
| V2DAY | (V2 field day) | busy street; scripted walker leaves on boresight (r = -13.8 + 1.635t, az 0-3°) | walker coverage in heavy real traffic; ghost trend (truth incomplete) | 16 dB | day | yes |
| radar4 | 20260714T033841Z | walker 69→30 m stop-and-go, then recedes 40→52 m at az ≈ -37°; far-clutter ghost bands at 100-180 m | ghost killers must not eat a stop-and-go walker; far clutter must stay dead | 16 dB | night | yes |
| radar5 | 20260714T034025Z | walker approaching 68→28 m; parked car (idling) at 52-68 m az 35-80°; multipath reflection ghosts — THE ghost scene | the ghost metric itself: reflections and parked-car mirrors must not become tracks | 16 dB | night | yes |

## What we still need (exact scene scripts)

- **Crossing human**, walking sideways at 30 / 80 / 150 m — day AND night.
  Proves the ghost killers spare crossers (tangential, Doppler ≈ 0).
- **Parked car, nobody moving**: engine off 5 min, then idling 5 min.
  The pure ghost source, isolated.
- **Empty street or field, 5 min** — an open-air negative. All current
  negatives are garages.
- **Two people walking together**, same distance, different bearings.
  Proves the copy-killer spares real pairs (same range, different az is the
  mirror signature).
- **Night walk PAST 320 m, no far-end pause** — extends the range truth
  beyond the T7 turnaround.

## The rule

Every new recording gets a row in this table **and** a truth sidecar
(`radar/tools/regression/truth/`) the same day it is taken, and joins
`tracker_gates.py` automatically via the manifest.
