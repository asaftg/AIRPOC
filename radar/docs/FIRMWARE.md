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

> Pitfall (from our own repo history): this firmware only accepts **one full cfg
> per power-cycle** — re-pushing after `sensorStop` gets "Reconfig not
> supported" and some lines are rejected. In production (boot → push once)
> that's clean; on a daemon restart without a radar power-cycle the sensor keeps
> streaming the loaded cfg and the re-push's unchanged lines are rejected
> harmlessly.

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
| Frame period | 50 ms → 20 Hz |

**Publish-max, filter-in-GUI.** The cfg is deliberately *permissive* — CFAR at
its 17 dB floor and FOV at full span — so the chip emits the **most** points it
safely can. The GUI trims SNR and azimuth **live with sliders** (host-side, no
cfg re-push). There are two SNRs and two FOVs by design: the **cfg** ones (CFAR
17 dB, FOV ±90) set what the chip *emits*; the **GUI** ones filter what's
*shown*. So the GUI needs only one SNR slider and one FOV slider — and **both
work today** (every point carries `snr` in dB and `az`).

Human baseline on this profile: a walking person is visible to **~100 m**
(dynamic returns only). Vehicles/drones reach farther (larger RCS).

## Frame rate (>20 Hz)

Active chirping is only ~30.3 ms of the 50 ms frame. Tightening `frameCfg`'s
period toward the active dwell (→ ~35 ms, **~28–30 Hz**) with **N=384 and 128
loops unchanged** raises the rate at **no cost to integration gain or Doppler
resolution** — so human range is unaffected. The limit is on-chip DSP time
per frame; HW-verify the shortest period that holds **0 dropped frames** (watch
`/stats` `drops`) and set that as the shipped period.

> Do **not** raise fps by cutting `numLoops`/`numAdcSamples` — that trades away
> the very gain that holds a weak, slow human at range.
