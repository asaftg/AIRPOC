# Radar firmware & profile

## Firmware

Custom **`mmw_demoDDM`** build (TI mmWave mcuplus SDK 4.7.2.1) for the
AWR2944P. On-chip range/Doppler FFT + CFAR + AoA; emits the mmw_demo TLV
point cloud over the data UART.

The chip runs the **V2 `agv2` image** (sha `173f622a`; binary + exact sources
packaged at [`radar/v2/fw/`](../v2/README.md)). On top of the V1 interleave
base it adds: the **overload crash fix** — a frame exceeding the UART budget
is deferred + counted instead of silently bricking the chip (450-pt frame
clamp, ISR-safe crash record in `queryDemoStatus`) — the DDMA **empty-band
comb gate** (`emptyBandGateCfg` CLI; see the pitfall below), and periodic RF
calibration re-enabled.

> Pitfall: `emptyBandGateCfg` must appear **after** the doppler `cfarCfg` line
> in the cfg (`cfarCfg` memsets the struct — earlier placement is silently
> off). Known open: the gate currently does **not** activate at runtime even
> when enabled — root-cause in progress; do not rely on it
> (see [`ROADMAP.md`](ROADMAP.md)).

Emits TLVs **{1, 7, 6, 9}** — DetectedPoints (1), **SideInfo / per-point SNR
(7)**, stats (6), temperature (9). (The range-profile TLV (2) is switched off
in the V2 cfg — nothing consumed it, ~10 KB/s reclaimed.)

**Per-point SNR is live** (verified on-board 2026-07-02: ~16–50 dB, floored at
the CFAR threshold). The DDM chain computes it (signal peak − CFAR noise, in
0.1 dB) and the firmware transmits **TLV 7** — the control is one `guiMonitor`
flag:

> `guiMonitor` field 2 = `detectedObjects`: **`1` = points WITH SideInfo/SNR**,
> `2` = points WITHOUT. The shipped `ag.cfg` uses **`1`**. (We shipped `2`
> initially and mislabelled it "firmware doesn't support SNR" — wrong; `2` is
> the value that *suppresses* it. No rebuild/reflash was needed.)

> Pitfall: this build does **not** link TI's Group Tracker (no TLV 308/309), so
> target boxes come from the host tracker, not the chip. The parser already
> recognises 308/309, so a future gtrack firmware drops straight in
> (a longer-horizon item — see [`ROADMAP.md`](ROADMAP.md)).

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
| Doppler CFAR | **16.0 dB** (V2; V1 shipped 17.0 — the crash the lower floor used to trigger is fixed in `agv2`; ladder to ~13 dB planned once the comb junk is gone) |
| Compression | BFP **0.75** (V2; 0.5 quantized away a weak target within ~21 m of a much stronger reflector). Must stay **on** — chip CFAR dies without it |
| MIMO | DDMA, 4TX/4RX |
| Azimuth FOV | **±90°** in cfg (full span; useful AoA ~±60°) |
| LVDS | **off** (`lvdsStreamCfg -1 0 0 0`) |
| Frame period | **38 ms → 26 Hz** (zero-compromise dead-time trim; see docs/FRAMERATE.md) |

**Publish-max, filter-in-host.** The cfg is deliberately *permissive* — CFAR
near its floor and FOV at full span — so the chip emits the **most** points it
safely can. The host tracker/GUI trims SNR, azimuth, and elevation **live**
(`/ctl` knobs, no cfg re-push). There are two SNRs and two FOVs by design: the
**cfg** ones set what the chip *emits*; the **live** ones filter what's
tracked/shown.

Measured envelope on this profile (V2, field): human ~**300 m** night-quiet /
~**200 m** day-busy (scene-noise limited); vehicle radial echoes to ~**424 m**.
Crossing (tangential) traffic is Doppler-blind — the Phase-3 item
(see [`ROADMAP.md`](ROADMAP.md)).

## Frame rate — DSP-bound at 26 Hz, and the chip reports its own margin

The profile is **DSP-bound**: `dsp_proc_ms` ≈ 17.07 ms/frame (measured from
the chip's stats TLV, surfaced in `/stats` along with `dsp_margin_ms` —
**margin trending to 0 is the frame-rate ceiling, measured not guessed**).
26 Hz is the shipped zero-compromise rate (dead-time trim only); anything
higher is firmware work and is parked. Full analysis, the rate ladder, and the
firmware path: [`FRAMERATE.md`](FRAMERATE.md).

> Do **not** raise fps by cutting `numLoops`/`numAdcSamples` — that trades away
> the very gain that holds a weak, slow human at range. Shorten idle/period, or
> accept a lower rate; never cut the integration.
