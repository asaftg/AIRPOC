# Radar firmware & profile

## Firmware

Custom **`mmw_demoDDM`** build (TI mmWave mcuplus SDK 4.7.2.1) for the
AWR2944P. On-chip range/Doppler FFT + CFAR + AoA; emits the mmw_demo TLV
point cloud over the data UART.

Emits TLVs **{1, 2, 6, 9}** — DetectedPoints (1), range profile (2), stats
(6), temperature (9).

> Pitfall: this build does **not** emit **TLV 7 (SideInfo / per-point SNR)**
> even with `guiMonitor ... detectedObjects 2`, and does **not** link TI's
> Group Tracker (no TLV 308/309). So per-point SNR is unknown (parser sets it
> `null`) and target boxes come from the host clusterer, not the chip. Both
> are Phase-2 firmware items (see plan) — the parser already recognises TLV 7
> and 308/309 so a future firmware drops straight in.

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
| CFAR floor (range) | 17 dB |
| MIMO | DDMA, 4TX/4RX |
| Azimuth FOV | ±30° |
| LVDS | **off** (`lvdsStreamCfg -1 0 0 0`) |
| Frame period | 50 ms → 20 Hz |

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
