# Radar frame rate — how high can we go, and what it costs

## The two clocks

Every frame runs two clocks back-to-back; the frame period must cover both:

- **Active ("listen")** = the chirping = `numLoops × 6 chirps × (idleTime + rampEndTime)`.
- **DSP ("think")** = the on-chip processing (Doppler FFT + DDMA demod + CFAR +
  AoA). **Measured from the chip's stats TLV: 17.07 ms/frame.**

`period ≥ active + DSP`. Frame period never touches range, range-resolution, or
sensitivity — those are set by slope / bandwidth / ADC / dwell.

## Measured baseline (2026-07-04, from the chip)

| | Active | DSP | Min period | Max rate |
|---|---|---|---|---|
| idle 12 / ramp 27.5 (original) | 30.3 ms | 17.07 ms | 47.4 ms | ~21 Hz |
| **idle 3 / ramp 20.5 (trimmed, shipped)** | **18.05 ms** | 17.07 ms | 35.1 ms | **~28.5 Hz** |

25 Hz and 30 Hz produced **zero frames** at the original timing — active+DSP
exceeded the period. Confirmed: the profile is DSP-bound and 20 Hz was already
~1 Hz under the ceiling.

## Non-negotiable constraints

- **500 m range** → `numAdcSamples 384`, slope 4.5, 30 MHz sampling LOCKED.
- **No AoA / elevation loss** → keep 4-TX DDMA + full antenna array.
- **No integration loss** → keep `numLoops 128`.

## The ladder (what each rate costs)

| Target | How | Compromise | Needs |
|---|---|---|---|
| **26–27 Hz** | **cfg trim: idle 12→3 µs, ramp 27.5→20.5 µs** (reclaim dead time) | **none** | cfg push only ← **SHIPPED** |
| 30 Hz (clean) | cfg trim + firmware shaves ~2 ms off the 17 ms | none | 1 fw rebuild + reflash |
| 30 Hz (cfg fallback) | cfg trim + `numLoops 128→96` | −1.25 dB integration, velocity-res ×1.33 | cfg only |
| 40 Hz (clean) | cfg trim + firmware cuts 17 ms → <7 ms | none *if the fw cut lands* | fw project, stretch |
| 50–60 Hz | — | **impossible** at full dwell (active 18 ms alone > 16.7 ms period) | — |

### Why the trim is free (grounded in the SDK)

`rampEndTime` floor = `adcStartTime(7) + 384/30MHz(12.8) = 19.8 µs` — the ADC
window closes there, so 27.5 µs carried ~7.7 µs of dead ramp. `idleTime` floor
= 2.5 µs (synth reset). Trimming to ramp 20.5 / idle 3 keeps the ADC window
fully inside the ramp → sampled BW 57.6 MHz, range 500 m, range-res 2.6 m all
unchanged. Only dead time is removed.

## Firmware path (PARKED — not worth it for 40–60 Hz)

Architecture (source-verified): the 17 ms runs on the **DSS Cortex-M4 @ 200 MHz**
(Doppler-HWA orchestration + CFAR-intersect), AoA on the R5F. The azimuth FFT is
already on the HWA; AoA is a cheap single-bin DFT — **not** the bottleneck. So
firmware *could* cut the 17 ms with no integration/AoA cost. But:

- **60 Hz is physically impossible** at full dwell; **50 Hz** needs an 8.7× DSP
  cut (unreal); **40 Hz** needs 2.5× (a big, unproven fw project); **30 Hz**
  needs only ~2 ms (doable but marginal over 26–27).

**Verdict: park firmware.** It never delivers 40–60 Hz clean, and 30 Hz clean is
a small gain for a reflash. Revisit only if 30 Hz becomes a hard requirement.

### If firmware is ever revisited — Phase 0 first (measure the 17 ms split)

The DPC already timestamps every stage on the M4 cycle counter; the
instrumentation is present but disabled. To surface the Doppler / CFAR-intersect
/ AoA breakdown: either widen the stats TLV with 3 extra `CycleCounterP` deltas
(our daemon already parses the stats TLV — clean), or flip
`OBJECTDETHWA_PRINT_DPC_TIMING_INFO` in `objectdetection.c` for a console print
(add `OBJECTDETHWA_TIMING_CPU_CLK_FREQ_KHZ 200000` under `SOC_AWR2X44P` or it
won't compile). **Requires a rebuild + reflash** — it is firmware work.

That split decides the fw lever: if M4-serial-bound → move the datapath to the
idle **C66x DSP** (bigger effort, the real lever); a naive "raise the M4 clock"
is likely void (200 MHz may be the M4 ceiling — verify the datasheet). Prune
unused TLV outputs for a small `transmitOutputTime` win regardless.

## Live health

`/stats` reports `dsp_proc_ms` (the 17 ms) and `dsp_margin_ms` (spare time). If
margin trends toward 0, the period is too aggressive. At 26 Hz / 38 ms the
expected margin is ~2.9 ms — the same headroom the proven 20 Hz ran on.
