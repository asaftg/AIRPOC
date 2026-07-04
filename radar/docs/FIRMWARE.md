# Radar firmware & profile

## Firmware

Custom **`mmw_demoDDM`** build (TI mmWave mcuplus SDK 4.7.2.1) for the
AWR2944P. On-chip range/Doppler FFT + CFAR + AoA; emits the mmw_demo TLV
point cloud over the data UART.

Emits TLVs **{1, 7, 2, 6, 9}** — DetectedPoints (1), **SideInfo / per-point SNR
(7)**, range profile (2), stats (6), temperature (9).

**Per-point SNR is live** (verified on-board 2026-07-02: ~16–50 dB, floored at
the CFAR threshold). The DDM chain computes it (signal peak − CFAR noise, in
0.1 dB) and the firmware transmits **TLV 7** — the control is one `guiMonitor`
flag:

> `guiMonitor` field 2 = `detectedObjects`: **`1` = points WITH SideInfo/SNR**,
> `2` = points WITHOUT. The shipped `ag.cfg` uses **`1`**. (We shipped `2`
> initially and mislabelled it "firmware doesn't support SNR" — wrong; `2` is
> the value that *suppresses* it. No rebuild/reflash was needed.)

> Pitfall: this build does **not** link TI's Group Tracker (no TLV 308/309), so
> target boxes come from the host clusterer, not the chip. On-chip gtrack is the
> one remaining firmware item — the parser already recognises 308/309 so a
> future firmware drops straight in.

> Pitfall (verified 2026-07-02): this firmware accepts **one full cfg per
> power-cycle**. A re-push starts with `sensorStop` (stops the sensor) and the
> reconfig + `sensorStart` are then rejected — leaving the sensor **stopped and
> unrecoverable without a power-cycle** (a bare `sensorStart` does not revive
> it). This is NOT harmless. The daemon therefore **reads first**: it peeks the
> data port ~2.5 s and only pushes the cfg if the port is silent (fresh boot);
> if the chip is already streaming it skips the push entirely, so a daemon
> restart never stops the sensor. Only a real power-cycle needs a fresh push.

## Profile — `cfg/awr2944P_ag.cfg` (A/G long-range, DCA-free)

`profileCfg 0 77 7 7 27.5 0 0 4.5 0 384 30000 0 0 164`

| Quantity | Value |
|---|---|
| Start freq | 77 GHz |
| Slope | 4.5 MHz/µs |
| ADC samples / rate | 384 @ 30 MHz |
| Sampled BW | 57.6 MHz |
| **Range resolution** | **2.60 m** |
| **Max range** | **500 m** |
| Integration gain | 25.8 dB (10·log₁₀ 384) |
| CFAR floor (range) | **17 dB — the minimum** (chip floods/collapses below) |
| MIMO | DDMA, 4TX/4RX |
| Azimuth FOV | **±90°** in cfg (full span; useful AoA ~±60°) |
| LVDS | **off** (`lvdsStreamCfg -1 0 0 0`) |
| Frame period | **40 ms → 25 Hz** (30 Hz measured infeasible at full dwell) |

**Publish-max, filter-in-GUI.** The cfg is deliberately *permissive* — CFAR at
its 17 dB floor and FOV at full span — so the chip emits the **most** points it
safely can. The GUI trims SNR and azimuth **live with sliders** (host-side, no
cfg re-push). There are two SNRs and two FOVs by design: the **cfg** ones (CFAR
17 dB, FOV ±90) set what the chip *emits*; the **GUI** ones filter what's
*shown*. So the GUI needs only one SNR slider and one FOV slider — and **both
work today** (every point carries `snr` in dB and `az`).

Human baseline on this profile: a walking person is visible to **~100 m**
(dynamic returns only). Vehicles/drones reach farther (larger RCS).

## Frame rate — the real limit is DSP margin, and the chip reports it

Raising fps by shortening the frame period is safe for detection: range,
range-resolution, integration gain and velocity-resolution are all set by
slope / bandwidth / ADC / dwell — **not** by frame timing. Field 6
(framePeriodicity) alone never touches them.

The only thing shortening the period spends is **inter-frame DSP time**. Each
frame the chip must finish range/Doppler FFT + CFAR + AoA in the gap between the
end of active chirping and the next frame. The chip **measures this itself** and
ships it in the **stats TLV (type 6)** — `interFrameProcessingTime` and
`interFrameProcessingMargin`. The daemon now parses it and exposes it in
`/stats` as **`dsp_proc_ms`** and **`dsp_margin_ms`** (plus `active_cpu_pct` /
`interframe_cpu_pct`). **`dsp_margin_ms` going to zero is the frame-rate
ceiling — measured, not guessed.**

The frame budget:

| Config | Active dwell | Period 33.33 ms → gap for DSP |
|---|---|---|
| idle 12 µs | 30.3 ms | ~3.0 ms |
| idle 7 µs  | 26.5 ms | ~6.8 ms |

**Measured 2026-07-04:** 30 Hz produced **no frames at both idle settings** —
idle 12 µs (~3 ms gap) *and* idle 7 µs (~6.8 ms gap). Both pushed cleanly and
the chip accepted the cfg, but the data port stayed silent: the DSP interframe
time exceeds even 6.8 ms, so the margin is negative and the frame engine never
launches. **25 Hz (40 ms, ~13.5 ms gap) is the shipped rate** — full dwell, all
integration gain, streams with margin. Read `dsp_proc_ms`/`dsp_margin_ms` from
`/stats` at 25 Hz to know the *exact* DSP time; the max frame rate for a given
dwell is then `1000 / (dwell_ms + dsp_proc_ms)`, no trial and error. To push
past 25 Hz without cutting chirps, trim the wasted ramp (see below) — check the
chip's interchirp margin first.

**40 Hz** (25 ms period) would need the dwell cut below 25 ms — i.e. fewer
Doppler loops — which *does* cost velocity resolution + ~1–3 dB sensitivity
(range and range-resolution still unaffected). Only worth it for fast movers.

> Do **not** raise fps by cutting `numLoops`/`numAdcSamples` — that trades away
> the very gain that holds a weak, slow human at range. Shorten idle/period, or
> accept a lower rate; never cut the integration.
