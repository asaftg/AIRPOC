# nv_imx296 — Driver Reference

Production tegracam V4L2 driver for the **Waveshare IMX296-130** (Sony IMX296, mono
global shutter) on the **Jetson Orin Nano** (P3768 / P3767-0005), JetPack 6.2.2 /
L4T r36.4.4. Streams **Y10 mono 1440×1088 @ 60 fps** with working exposure/gain
v4l2 controls. Source: [`eo/driver/`](../driver).

## Configuration facts

| Fact | Value | Why |
|---|---|---|
| Input clock (INCK) | **54 MHz, on-board crystal** | The Waveshare board self-clocks; the Jetson mclk is ignored. INCKSEL `{0xb0,0x0f,0xb0,0x0c}`, CTRL418C `0xa8`. |
| Sensor pixel clock | **74.25 MHz** = HMAX·VMAX·fps (1100·1125·60) | Governs SHS1/VMAX line timing. **Not** the DT `pix_clk_hz` (118.8 MHz MIPI byte rate). |
| Output | Y10, left-justified in 16-bit | `>>6` for the 10-bit value. Overlay `pixel_t = "mono_y10"`. |
| Active window | **1440×1088** (ROI-cropped from 1456) | 64-byte line alignment — see *Comb* below. |
| i2c | addr `0x1a` | bus: `ls -d /sys/bus/i2c/devices/*-001a` |

## Register map (mainline `drivers/media/i2c/imx296.c`)

| Register | Addr | Width | Notes |
|---|---|---|---|
| SHS1 (shutter) | `0x308d` | 24-bit LE | `SHS1 = VMAX − exposure_lines`; smaller = brighter; line = 14.815 µs |
| GAIN | `0x3204` | 16-bit LE | 0–480, 0.1 dB/step |
| VMAX (frame length) | `0x3010` | 24-bit LE | 1125 for 60 fps |
| HMAX (line length) | `0x3014` | 16-bit LE | 1100 |
| CTRL08 REGHOLD | `0x3008` | bit0 | atomic latch for SHS1+GAIN (see below) |
| FID0_ROI + ROIWH1 | `0x3300` / `0x3314` | — | `0x03` + width `1440` → the 1440 crop |
| MIPIC_AREA3W | `0x4182` | 16-bit LE | 1088 MIPI active height (Tegra needs it or VI times out) |

## Exposure / gain semantics

- **Exposure**: v4l2 `exposure` in µs. `coarse_lines = exp_us × 74.25 / line_length`;
  `SHS1 = VMAX − coarse − 1`. At 60 fps, max integration ≈ **16.5 ms** (SHS1 → 8).
- **Gain**: v4l2 `gain` 0–480 → `0x3204` (0.1 dB/step, exponential brightness).
- **REGHOLD**: writes to SHS1/GAIN are bracketed by `CTRL08` bit0 (=1, write both,
  =0) so a frame never latches a half-written 24-bit SHS1. Implemented in
  `set_group_hold` and in the preview tool's i2c AE.

## Even/odd comb — why the sensor is cropped to 1440

> Pitfall: the Tegra VI aligns every captured line to a **64-byte** boundary. The
> native 1456-px Y10 line is 2912 bytes (`mod 64 = 32`), so the VI pads odd lines by
> 16 px → an even/odd "comb". Changing only the VI capture width breaks streaming
> (VI/CSI line-length mismatch → `corr_err`). Fix: crop the **sensor** to 1440 via
> `FID0_ROI` so its MIPI line is 2880 bytes (= 45×64, aligned) and the VI matches it.
> Do not widen back to 1456.

## Building the module (on-device)

`nvbuild.sh -m` and whole-tree builds fail on unrelated modules; build **only** the
i2c directory:

```bash
S=~/imx296-bringup/Linux_for_Tegra/source
make -C /lib/modules/$(uname -r)/build \
  M=$S/nvidia-oot/drivers/media/i2c \
  KCFLAGS="-I$S/out/nvidia-conftest -I$S/nvidia-oot/include" \
  KBUILD_EXTRA_SYMBOLS=$S/nvidia-oot/Module.symvers \
  modules
sudo cp $S/nvidia-oot/drivers/media/i2c/nv_imx296.ko \
        /lib/modules/$(uname -r)/updates/drivers/media/i2c/
sudo depmod -a
```

The overlay is compiled from `...imx296-C.dts` and selected in
`/boot/extlinux/extlinux.conf` (`OVERLAYS .../imx296-C.dtbo`).

## Bring-up pitfalls (so they don't recur)

> - Bind **our** driver, not a prebuilt `imx296.ko` — prebuilts can accept v4l2
>   exposure/gain writes that never reach the sensor (dark, uncontrollable image).
> - Seed `priv->frame_length = 1125` in `set_mode`: the framework applies EXPOSURE
>   before FRAME_RATE, so an unseeded `frame_length` makes SHS1 underflow.
> - Timing math uses the **74.25 MHz sensor pixel clock**, not the DT MIPI byte rate.
> - Advertise mono **Y10** (`pixel_t = "mono_y10"`), not Bayer RG10 — otherwise the
>   VI silently drops frames.
