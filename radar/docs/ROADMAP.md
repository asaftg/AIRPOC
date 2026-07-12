# Radar module — versions & roadmap

The radar ships as locked versions; each is a complete revert point (fw image +
flash cfg + chip cfg + tracker sources) packaged in the repo. This doc is the
map: what each version is, what is open, and what comes next. Detail lives in
the linked docs — don't duplicate it here.

| Version | State | Package |
|---|---|---|
| V1 | frozen 2026-07-10 (tag `AIRPOC-RADAR-V1.0`) | [`radar/v1/`](../v1/README.md) · [`VERSION_V1.0.md`](../VERSION_V1.0.md) |
| V2 | **shipped 2026-07-11** — flashed + field-verified | [`radar/v2/`](../v2/README.md) · [`SHIP_RUNBOOK_V2.md`](SHIP_RUNBOOK_V2.md) |
| V3 | planned (three items below) | [`PHASE3_ANGLE_MOTION_SPEC.md`](PHASE3_ANGLE_MOTION_SPEC.md) |

## V1 (frozen) — the known-good baseline

The stack as field-used 2026-05-22 → 2026-07-10: interleave fw (sha
`7292938f`, stock TI DDM datapath), 17.0 dB doppler CFAR / compression 0.5
cfg, tracker with a hard −9°…+2.5° elevation window and no consistency guard.
Verified: vehicles ~380 m radial, humans ~230 m (350 m best night), 26.3 Hz /
0 drops. Known faults it shipped with: the chip **silently bricked** when a
frame exceeded the UART budget, ~95 DDMA-comb ghost movers/frame, wandering
immortal tracks in multipath clutter, level-mount-only elevation gating.

## V2 (shipped 2026-07-11) — crash-proof fw + guarded tracker

**Firmware `agv2`** (sha `173f622a`): the overload crash fix — a frame that
exceeds the UART budget is now **deferred + counted** instead of killing the
chip (450-pt frame clamp, ISR-safe crash record) — plus the DDMA **empty-band
comb gate** (new `emptyBandGateCfg` CLI, shipped dark). Field-verified: survived
the exact car-drive-by stimulus that bricked V1-era fw twice (434 pts/frame
peak, 0 deaths).

**Chip cfg**: doppler CFAR 17.0 → **16.0 dB**, compression 0.5 → **0.75**
(closes the weak-near-strong quantization hole), rangeProfile TLV off.

**Tracker (SW)**: post-confirm **consistency guard** (commits `65cb276` +
`6b24d7e`) — wandering/ghost tracks die (0/0/0 on the junk fixtures), coherent
movers and standing far targets survive, faint-far targets get a
doppler-consistency relief. The hard elevation window is gone — elevation
gating is the live `elmax` knob (default ±20°, gimbal-safe). cfg push hardened
(every line must ack `Done`).

**Measured envelope** (operator-confirmed field sessions): human ~**300 m**
night-quiet / ~**200 m** day-busy — scene-noise limited, not sensor-limited;
vehicle radial echoes to ~**424 m**. Crossing (tangential) traffic is still
blind — physics (Doppler ≈ 0), the Phase-3 item.

**Known open (V2):**
- **The comb gate does not activate at runtime** — enabling `emptyBandGateCfg`
  has no observed effect; root-cause analysis is in progress. Until it works,
  junk points remain ~250/frame at 16 dB (the guard keeps the *emitted tracks*
  clean; the junk loads the tracker input and the UART budget).
- Logged ship acceptances: tangential queue/junction traffic weak until
  Phase 3; far-standing drift >15 m only partially held; +0.3 s confirm
  latency (EO remains the fast channel).

## V3 / next — three items, in order

1. **Fix + calibrate the comb gate.** Root-cause why the gate is inert, then
   the enablement recipe already written: corner-reflector LSB/dB calibration,
   margin sweep 3/6/12/24 dB on a comb-heavy scene, A/B vs gate-off on the
   regression corpus — [`SHIP_RUNBOOK_V2.md`](SHIP_RUNBOOK_V2.md) step 7.
2. **Bar-ladder max-range session.** 16 → 15 → 14 → 13 dB, same walk each
   step, gate ON first (junk flood at 14/13 dB can evict weak far targets via
   the 450-pt clamp) — [`SHIP_RUNBOOK_V2.md`](SHIP_RUNBOOK_V2.md) step 8,
   scored with `tools/walkout_score.py`.
3. **Phase 3 — on-chip angle-motion detector** for crossing traffic (the
   tangential blindness). Full implementation spec:
   [`PHASE3_ANGLE_MOTION_SPEC.md`](PHASE3_ANGLE_MOTION_SPEC.md).

The firmware-side analysis behind V2 + Phase 3 (root causes, cfg wave, review
verdicts) is preserved in [`AG_FW_PLAN.md`](AG_FW_PLAN.md) (historical detail).

## Longer horizon (unordered, unchanged in value)

- **Per-unit antenna calibration** — `antennaCalibParams` are TI sample values
  (angle accuracy gap for standalone guidance). Corner-reflector
  `measureRangeBiasAndRxChanPhase` bench, gated like an incident
  (AG_FW_PLAN item C6).
- **On-chip Group Tracker (gtrack)** — chip emits target boxes (TLV 308/309,
  parser already handles them), host tracker retires. Optional.
- **Learned point-cloud detection** — real upside for faint humans/drones;
  blocked on collecting + labelling our own recordings (no usable public
  radar data). The regression corpus discipline
  ([`TEST_CORPUS.md`](TEST_CORPUS.md)) is the on-ramp.
- **Re-tune across more scenes** — every change re-validated against the full
  fixture set before shipping ([`TUNING.md`](TUNING.md) re-tuning loop).

## Known constraints (don't re-discover these)

- **One cfg per power-cycle** — the daemon reads-first and won't re-push to a
  live chip; a stuck sensor needs a radar power-cycle (see
  [`FIRMWARE.md`](FIRMWARE.md)).
- **No DCA1000** — raw-ADC / micro-Doppler paths are physically impossible on
  this build; radar is the UART point cloud only.
- **Frame rate is DSP-bound at 26 Hz** — firmware-only above that; parked
  (see [`FRAMERATE.md`](FRAMERATE.md)).
- **Per-point SNR** is a `guiMonitor detectedObjects` flag (1 = with), not a
  firmware feature to build.
