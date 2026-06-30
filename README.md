# AIRPOC — Jetson Orin Nano + IMX296 camera

Project bench: **NVIDIA Jetson Orin Nano Super** (JetPack 6.2.2 / L4T r36.4.4, kernel `5.15.148-tegra`) + **Waveshare IMX296-130** mono global-shutter MIPI camera, headless.

## Layout
- `docs/JETSON_ORIN_NANO_BRINGUP.md` — full OS bring-up + camera recipe + the root-cause story.
- `jetson/camera/` — custom `nv_imx296` Tegracam driver, mode tables, device-tree overlay (CAM1 / serial_c), and the **Y10 VI patch** (`vi5_formats_y10.patch`).
- `jetson/fan/` — always-100% fan service (`jetson-fan-max.service`).

## Camera status
Streams **Y10 mono 1456×1088 @ 60 fps** (real images verified). The long bring-up's root cause: the **stock Tegra VI silently drops mono Y10 frames** — not the clock, ribbon, or hardware. Fixed by adding a mono-Y10 entry to the VI format table (`vi5_formats_y10.patch`) + the mono Y10 capture path. Known-good reference: [INNO-MAKER/cam-imx296raw-trigger](https://github.com/INNO-MAKER/cam-imx296raw-trigger).

Working overlay values: `num_lanes=1`, `lane_polarity=0`, `discontinuous_clk=no`, `mclk_khz=54000` (37.125 also fine), `line_length=1100`, `pix_clk_hz=118800000`, `embedded_metadata_height=2`. Driver writes the Tegra-specific `MIPIC_AREA3W (0x4182)=1088`.

> Note: capture is **Y10 left-justified** (10-bit data in the high bits of the 16-bit word — shift `>>6` to read raw values). Production clean-driver + mono ISP/AE in progress.
