# Jetson Orin Nano Super — OS Bring-up & IMX296 Camera

End-to-end bring-up of the **Jetson Orin Nano Super dev kit** (module **P3767-0005**, carrier **P3768**) on **JetPack 6.2.2 / L4T r36.4.4** (kernel `5.15.148-tegra`), headless, plus the **Waveshare IMX296-130** mono global-shutter MIPI camera.

---

## 1. Hardware
- **Module/carrier:** P3767-0005 / P3768 (Orin Nano Super, 8 GB).
- **Display:** DisplayPort-only (no HDMI). We ran **fully headless** — no monitor.
- **Camera:** Waveshare IMX296-130 (1.6 MP mono global shutter, RPi-style, **no on-board oscillator**) on the **CAM1** 22-pin CSI connector.
- **Storage:** microSD (JP6 image) + an NVMe SSD (wiped, scratch).

## 2. OS install (the key lesson)
The Super ships with **JP6-era firmware**. A **JetPack 5 SD card halts ~10 s into boot** (R36 bootloader cannot boot an R35 rootfs) — this looks like a dead board (fan + ethernet die, power LED stays on) but is just the wrong card. **For a Super, flash JetPack 6 directly** — do NOT use the JP5.1.3 "bridge."

- Image: `jetson-orin-nano-devkit-super-SD-image_JP6.2.x` (L4T r36.4.4).
- **Write with Etcher (native Windows)** — NOT `dd` over usbipd (sustained 24 GB writes drop the flaky USB card reader mid-write and corrupt the image).
- Verify the zip CRC before flashing (a resumed/partial download corrupts it).

## 3. Headless provisioning (no console)
Edit the mounted rootfs offline (scripts in `scripts/`), via WSL2 + **usbipd** (the card reader is removable media; `wsl --mount` rejects it):
- Create user `asaftg` (passwd/shadow/group + `openssl passwd -6`).
- **Skip oem-config:** `ln -sf .../graphical.target etc/systemd/system/default.target`.
- Drop a NetworkManager `.nmconnection` for Wi-Fi.
- **Pre-generate SSH host keys** so `sshd` starts on first boot; set `PasswordAuthentication yes`.
- nvfb-early/nvfb still run (fix apt `<SOC>`→t234, gen keys).

**Dead ends — do not repeat:** SDK Manager flash-over-WSL fails at the OS-write step (needs USB-networking/NFS WSL can't do). RCM/QSPI upload works but the OS copy doesn't.

## 4. First-boot setup
- **rootfs does NOT auto-expand** (skipping oem-config skips the resize) — it boots 21 G and fills to 100 %, breaking apt. Fix online:
  ```bash
  echo Yes | parted ---pretend-input-tty /dev/mmcblk0 resizepart 1 100%
  partx -u /dev/mmcblk0; resize2fs /dev/mmcblk0p1     # -> 116 G
  ```
- **Max performance:** `nvpmodel -m 2` (mode 2 = **MAXN_SUPER**; modes: 0=15W, 1=25W, 2=MAXN_SUPER, 3=7W — note 0 is the *slowest*), then `jetson_clocks`.
- **Compute stack:** `apt install nvidia-jetpack` → CUDA 12.6, TensorRT 10.3, cuDNN, VPI, OpenCV. Run detached (SSH blips don't interrupt).
- **NVMe:** `blkdiscard /dev/nvme0n1` to wipe (scratch).

## 5. Networking
- **Wi-Fi is stable** (`wlP1p1s0`); **Ethernet is flaky** (drops DHCP) — prefer Wi-Fi for internet.
- **Best control channel = USB device-mode net `192.168.55.1`** (host side `192.168.55.100`): rock-solid point-to-point over USB-C, immune to LAN flapping. SSH there for admin; internet rides Wi-Fi.
- The Jetson **does not answer ICMP** — find it by router DNS hostname `orin-nano`, not ping/scan.
- Access: `ssh asaftg@192.168.55.1` or `ssh asaftg@orin-nano` (pw `872002`).

## 6. Fan — forced 100 % always
The fan is a thermal cooling device (`pwm-fan`, states 0–3); the kernel governor idles it at state 2 (pwm 187) and overrides direct `pwm1` writes. To run it at 100 % at all times (the dev kit can hiccup when cold with the fan idled):
- `systemd` unit **`jetson-fan-max.service`** runs **`/usr/local/bin/fan-max.sh`**, which stops/masks `nvfancontrol`, then in a 1 s loop holds the cooling device at `max_state` (3) and writes `pwm1=255`.
- Result: steady **pwm 255**, never idles off, survives reboot. Files: `fan/fan-max.sh`, `fan/jetson-fan-max.service`, `fan/install_fan.sh`.

## 7. IMX296 camera — bring-up & the real root cause
There is **no native IMX296 support in JetPack 6** (only IMX219/477 ship overlays) — a custom driver is required. The bare RPi/Waveshare IMX296 had been **publicly unsolved on Jetson**; every working vendor module (VC, Leopard, INNO-MAKER) carries its own oscillator.

**Symptom:** sensor probes over i2c (0x1a), `/dev/video0` enumerates, sensor configures, but every capture → `tegra-camrtc-capture-vi: uncorr_err: request timed out after 2500 ms`, 0 bytes, **zero CSI errors**.

**The real root cause (after exhaustive elimination):** the IMX296 is mono → outputs **Y10 (10-bit greyscale)**, and the **stock Tegra VI (`tegra-camera.ko`) silently drops Y10 frames**. It is NOT the clock (the SoC's 37.09 MHz vs nominal 37.125 is a 0.09 % red herring — the Tegra `tegracam` driver doesn't enforce the exact-rate check the mainline RPi driver does), NOT the ribbon, NOT hardware.

**What works (confirmed streaming, 31.7 MB = 10 real frames, Y10 1456×1088 @ 60 fps):**
- A **Y10-patched `tegra-camera.ko`** + an `imx296.ko` that auto-detects mono.
- Reference/known-good: [INNO-MAKER/cam-imx296raw-trigger](https://github.com/INNO-MAKER/cam-imx296raw-trigger) (matches this exact kernel `5.15.148-tegra`).
- **Working overlay values** (`tegra234-p3767-camera-p3768-imx296mono-cam1.dtbo`): `num_lanes="1"`, `lane_polarity="0"`, `discontinuous_clk="no"` (continuous), `mclk_khz="54000"`, `line_length="1100"`, `pix_clk_hz="118800000"`, `embedded_metadata_height="2"`, `tegra_sinterface="serial_c"`.
- Tegra-specific init register the mainline lacks: **`MIPIC_AREA3W (0x4182) = 1088`**.

**Enable:**
```bash
sudo python3 /opt/nvidia/jetson-io/config-by-hardware.py -n '2=Camera IMX296LLR-Mono Y10 Cam1'
sudo reboot
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10 --stream-to=/tmp/c.raw   # ~31 MB
```

> Production note: a from-source clean driver + ISP/AE is the next step (INNO-MAKER ships binaries only). The Y10 VI support must be built into our oot `tegra-camera`, and the mono ISP/AE (no demosaic/AWB) ported from the seeker EO pipeline.
