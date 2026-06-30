# nv_imx296 — Driver Reference

Production Tegracam V4L2 driver for the **Waveshare IMX296-130** (Sony IMX296,
mono global-shutter) on the **Jetson Orin Nano** (P3768 / P3767-0005), JetPack
6.2.2 / L4T r36.4.4, kernel 5.15.148-tegra.

Streams **Y10 mono 1456×1088 @ 60 fps** with working exposure/gain v4l2 controls.

Files: `jetson/camera/nv_imx296.c`, `imx296_mode_tbls.h`,
`tegra234-p3767-camera-p3768-imx296-C.dts`.

## Sensor facts (the ones that mattered)

| Fact | Value | Why it matters |
|---|---|---|
| Input clock (INCK) | **54 MHz, on-board crystal** | The Waveshare board self-clocks; the Jetson `extperiph1` mclk is **ignored**. 37.125 MHz never brought MIPI up. |
| Sensor pixel clock | **74.25 MHz** = HMAX·VMAX·fps (1100·1125·60) | Governs SHS1/VMAX line timing. **Not** the DT `pix_clk_hz` (118.8 MHz = MIPI byte rate, used only for VI bandwidth). |
| Output | Y10, **left-justified** in 16-bit | Shift `>>6` for the 10-bit value, `>>8` for 8-bit. |
| i2c | addr `0x1a` | Bus: `ls -d /sys/bus/i2c/devices/*-001a`. |

## Register map (from mainline `drivers/media/i2c/imx296.c`)

| Register | Addr | Width | Notes |
|---|---|---|---|
| SHS1 (shutter) | `0x308d` | 24-bit LE | `SHS1 = VMAX − exposure_lines`; smaller = brighter |
| VMAX (frame length) | `0x3010` | 24-bit LE | 1125 for 60 fps |
| HMAX (line length) | `0x3014` | 16-bit LE | 1100 |
| GAIN | `0x3204` | 16-bit LE | 0–480, 0.1 dB/step |
| INCKSEL[0..3] | `0x3089–0x308c` | — | 54 MHz: `{0xb0,0x0f,0xb0,0x0c}` |
| GTTABLENUM | `0x4114` | 8-bit | `0xc5` |
| CTRL418C | `0x418c` | 8-bit | 54 MHz: `0xa8` (168) |
| MIPIC_AREA3W | `0x4182` | 16-bit LE | **1088** — Tegra-specific MIPI active height; without it the MIPI frame never forms → VI timeout |
| BLKLEVEL | `0x3254` | — | `0x3c` (60) — the black-level floor seen in raw |
| STANDBY / XMSTA | `0x3000` / `0x300a` | — | stream start clears both (master free-run) |

## Bring-up root causes (five stacked bugs)

The long bring-up was **not** the clock precision, the ribbon, or the hardware.
In order of discovery:

1. **Wrong driver bound.** INNO-MAKER's prebuilt `imx296.ko` (used as a
   diagnostic) was binding the sensor; its v4l2 exposure/gain controls accept
   values but never write the sensor. Fix: disable it, add `sony,imx296ll` to
   our `of_match` so `nv_imx296` binds the mono node.
2. **Clock.** 37.125 MHz INCKSEL never streamed. The board self-clocks at
   **54 MHz** → INCKSEL `{0xb0,0x0f,0xb0,0x0c}`, CTRL418C `0xa8`.
3. **SHS1 underflow.** The framework applies `EXPOSURE` before `FRAME_RATE`, so
   `priv->frame_length` was 0 when `set_exposure` ran → `SHS1 = 0 − coarse − 1`
   wrapped to ~1e6 (invalid) → exposure pinned. Fix: seed
   `priv->frame_length = 1125` in `set_mode`.
4. **Wrong clock in timing math.** `set_exposure`/`set_frame_rate` used the DT
   `pix_clk_hz` (118.8 MHz MIPI rate). SHS1/VMAX need the **74.25 MHz** sensor
   pixel clock. Fix: `IMX296_SENSOR_PIXEL_CLOCK = 74250000`.
5. **RG10 vs Y10.** Overlay enumerated as Bayer RG10. Fix: `pixel_t = "mono_y10"`.

Verified after all five: v4l2 exposure+gain sweep drives frame mean **60 → 974**.

## Live-AE band-shift tearing (the 6th issue) — REGHOLD

Separate from bring-up: when the AE changes exposure/gain **while streaming**,
frames showed horizontal "staircase" band-shift tearing. Root cause confirmed by
(a) an objective frame-to-frame diff test — **0/44 frame-pairs torn with zero
mid-stream writes** (no CSI/VI hardware errors), and (b) three independent
research passes converging on the same mechanism:

The IMX296 latches SHS1/GAIN/VMAX per frame. Writing them as bare i2c
transactions mid-stream lets a frame latch **partial or mismatched** timing
values (a half-written 24-bit SHS1, or SHS1/VMAX disagreeing for one frame) →
that frame is read out with inconsistent timing → band-shift tear. Mainline
`imx296.c` has the same latent bug (it `#define`s REGHOLD but never uses it).

**Fix — `CTRL08` (0x3008) bit0 = REGHOLD**, atomic latch:
```
0x3008 = 0x01            ; REGHOLD on (freeze shadow registers)
SHS1 @ 0x308d (24-bit)
GAIN @ 0x3204 (16-bit)
0x3008 = 0x00            ; REGHOLD off -> all latch together at next VSYNC
```
Implemented in `imx296_set_group_hold()` (the tegracam framework brackets
control writes with it), and in the preview tool's i2c AE (`_apply_exposure`).
Keep `VMAX > SHS1` inside the held block. This is the production-correct fix —
not a vblank-timing race.

> Note: a separate hypothesis (set `embedded_metadata_height` 2→0) was raised but
> deprioritized — the frame-diff showed the tearing is write-induced (dynamic),
> not a static stride/metadata shift, and changing metadata height risks the
> working stream. Revisit only if REGHOLD doesn't fully clear it.

## Controls — semantics

- **Exposure**: v4l2 `exposure` in **µs**, range 29–16000 (`exposure_factor=1e6`,
  `min/max_exp_time` in the overlay `mode0`). `coarse_lines = exp_us ×
  74.25e6 / 1e6 / line_length`; `SHS1 = VMAX − coarse − 1`.
- **Gain**: v4l2 `gain` 0–480 (`gain_factor=10`), written directly to `0x3204`
  (0.1 dB/step ⇒ exponential brightness).

## Building the module (on-device)

`nvbuild.sh -m` fails (missing `nvdisplay`); a whole-tree build dies on an
unrelated `gpu/drm` module. Build **only** the i2c directory:

```bash
S=~/imx296-bringup/Linux_for_Tegra/source
make -C /lib/modules/$(uname -r)/build \
  M=$S/nvidia-oot/drivers/media/i2c \
  KCFLAGS="-I$S/out/nvidia-conftest -I$S/nvidia-oot/include" \
  KBUILD_EXTRA_SYMBOLS=$S/nvidia-oot/Module.symvers \
  srctree.nvidia-oot=$S/nvidia-oot srctree.nvconftest=$S/out/nvidia-conftest \
  modules
sudo cp $S/.../i2c/nv_imx296.ko /lib/modules/$(uname -r)/updates/drivers/media/i2c/
sudo depmod -a
```

Overlay is selected in `/boot/extlinux/extlinux.conf` (`OVERLAYS .../imx296-C.dtbo`).

## Known remaining item (minor, modeling only)

The overlay still *names* `extperiph1` as the mclk source. It's harmless (the
sensor self-clocks, so the value is ignored), but the fully-correct model is a
`fixed-clock` node at 54 MHz with `mclk = "mclk"` (as the reference overlay
does). Functional behavior is identical; this is a from-source DT recompile
when convenient. See `STREAMING.md` / `ARCHITECTURE.md` for the bigger picture.

## Trigger / external sync

The driver runs the sensor in master free-run (`XMSTA`). For future NIR-PCB
sync, keep the `XVS`/`XHS`/`XTRIG` path available (slave/external-trigger mode).
See `NIR_SYNC.md`.
