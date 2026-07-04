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

`profileCfg 0 77 12 7 27.5 0 0 4.5 0 384 30000 0 0 164`

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
| Frame period | **50 ms → 20 Hz** (proven; 25/30 Hz gave no frames — DSP-bound, measuring) |

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

**Measured 2026-07-04:** both 30 Hz (33.33 ms) *and* 25 Hz (40 ms) produced
**no frames** — cfg pushed cleanly and the chip accepted it, but the data port
stayed silent. 20 Hz (50 ms) is the only rate proven to stream. That pattern
points to the profile being **DSP-bound**: the per-frame processing (N=384, 128
loops, DDMA 4-TX + CFAR + AoA) likely needs ~14–19 ms, so the real ceiling is
near ~22 Hz and 20 Hz was already close to it — *not* a duty-cycle margin issue
as first assumed. **Confirm from the chip:** run 20 Hz, read `dsp_proc_ms` from
`/stats`; the max sustainable rate is `1000 / (active_ms + dsp_proc_ms)`. If
that's ≤ ~22 Hz, going faster requires **less DSP load** (fewer loops/samples =
the integration trade), not a shorter period.

**40 Hz** (25 ms period) would need the dwell cut below 25 ms — i.e. fewer
Doppler loops — which *does* cost velocity resolution + ~1–3 dB sensitivity
(range and range-resolution still unaffected). Only worth it for fast movers.

> Do **not** raise fps by cutting `numLoops`/`numAdcSamples` — that trades away
> the very gain that holds a weak, slow human at range. Shorten idle/period, or
> accept a lower rate; never cut the integration.
