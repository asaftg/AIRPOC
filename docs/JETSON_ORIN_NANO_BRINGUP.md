# Jetson Orin Nano + IMX296 — Bring-up Checklist

Fresh Jetson Orin Nano Super dev kit → IMX296 mono camera streaming at
1456×1088 @ 60 fps, focused, auto-exposed, fan pinned. Work top to bottom.

> Targets: **JetPack 6.2.2 / L4T r36.4.4**, kernel `5.15.148-tegra`,
> module **P3767-0005**, carrier **P3768**, **Waveshare IMX296-130** (mono,
> global shutter, no on-board oscillator) on **CAM1 / CSI port C**.

## 1. Hardware
- [ ] Module seated, heatsink/fan mounted, NVMe or SD with capacity for JetPack.
- [ ] IMX296 ribbon → **CAM1** connector; contacts face the board, latch closed both ends.
- [ ] **Peel the lens protective film.** Hand-thread the M12 lens a few turns in.

## 2. Flash JetPack 6.2.2
- [ ] On an Ubuntu host: NVIDIA **SDK Manager** → target Orin Nano → JetPack 6.2.2.
- [ ] Put the board in **recovery** (jumper FC REC↔GND, power on), flash, let it complete.
- [ ] First boot → finish `oem-config` (user `asaftg`), connect network.
- [ ] `sudo nvpmodel -m 0 && sudo jetson_clocks` → **MAXN_SUPER**, clocks locked.
- [ ] Confirm: `cat /etc/nv_tegra_release` shows **R36 (release), REVISION: 4.4**.

## 3. Camera driver + overlay
From `jetson/camera/` in this repo (build on-device — modules are kernel-specific):
- [ ] Stage the L4T kernel / OOT source matching the running kernel.
- [ ] Build `nv_imx296.ko` (tegracam driver; mode table in `imx296_mode_tbls.h`) and install to `/lib/modules/$(uname -r)/updates/`.
- [ ] Install the overlay `tegra234-p3767-camera-p3768-imx296-C.dtbo` to `/boot/`.
- [ ] Ensure the **VI accepts mono Y10** (stock Tegra VI silently drops it). Production path: the driver advertises `MEDIA_BUS_FMT_Y10_1X10` itself, no core-tree change needed (see `ENGINEERING_GUIDELINES.md`).
- [ ] `sudo /opt/nvidia/jetson-io/config-by-hardware.py -n '2=IMX296 Mono Cam1'` → reboot.

## 4. Verify streaming
- [ ] `ls /dev/video0` exists.
- [ ] `v4l2-ctl -d /dev/video0 --list-formats-ext` → `Y10` (or `GREY`) `1456x1088`.
- [ ] `v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10 --stream-to=/tmp/c.raw`
- [ ] `ls -l /tmp/c.raw` → **≈31.7 MB** (10 frames × 1456×1088×2). 0 bytes / `VI request timed out` ⇒ VI is dropping Y10 → revisit step 3.

> **Y10 is left-justified** in the 16-bit word: shift `>>6` for the 10-bit value, `>>8` for 8-bit. Camera i2c is addr `0x1a`; find the bus with `ls -d /sys/bus/i2c/devices/*-001a`.

## 5. Exposure / gain
- [ ] Controls: **SHS1** `0x308d` (24-bit, `SHS1 = VMAX − exposure_lines`, smaller = brighter); **GAIN** `0x3204` (16-bit, 0–480, 0.1 dB/step); **VMAX** `0x3010`.
- [ ] Sanity check the sensor exposes: stream, then `i2ctransfer -f -y <bus> w5@0x1a 0x30 0x8d 0x08 0x00 0x00` (near-max exposure) → frame mean jumps bright. (The shipping driver exposes these as v4l2 controls.)

## 6. Focus + quality (tools — see `jetson/tools/`)
- [ ] Focus: `ssh asaftg@orin-nano 'bash ~/focus.sh'` → `http://orin-nano:8090`, turn the M12 ring until Tenengrad/Laplacian peak. (`FOCUS_TOOL_GUIDE.md`)
- [ ] Quality/AE preview: `sudo ~/preview.sh` → `http://orin-nano:8091` (auto-exposure + light ISP).

## 7. Fan always 100%
From `jetson/fan/`:
- [ ] `sudo ./install_fan.sh` → installs `jetson-fan-max.service`, masks `nvfancontrol`.
- [ ] Verify: `cat /sys/class/hwmon/hwmon*/pwm1` → **255**.

## 8. Done
- [ ] `/dev/video0` streams Y10 60 fps, image focused + auto-exposed, fan at 255, MAXN_SUPER. Commit any device-specific notes back to this repo (it is the single source of truth).
