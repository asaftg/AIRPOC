# Jetson Platform Bring-up

Fresh **Jetson Orin Nano Super** dev kit → a running, tuned compute platform ready
for the sensor modules. This chapter is the **Jetson only** — flashing, base
config, cooling.

> Target: **JetPack 6.2.2 / L4T r36.4.4**, kernel `5.15.148-tegra`, module
> **P3767-0005**, carrier **P3768**. Power mode **MAXN_SUPER**.

## 1. Flash JetPack 6.2.2

Two ways. **Use the SD-card image** unless you need to boot from NVMe.

### Method A — SD-card image (simplest)
1. On any machine, download the **Jetson Orin Nano Developer Kit SD Card Image** for
   **JetPack 6.2.2** from NVIDIA (`developer.nvidia.com/embedded/jetpack`). It's a
   `.img` (zipped, ~10 GB).
2. Flash it to a **≥64 GB microSD** (UHS-I / A2) with **Balena Etcher**: *Flash from
   file* → the image → select the SD → *Flash*. Etcher verifies automatically.
3. Insert the SD into the module, connect display/keyboard (or serial), power on.

### Method B — SDK Manager (for NVMe / no SD image)
1. On an **Ubuntu 20.04/22.04** host, install NVIDIA **SDK Manager**.
2. Put the board in **Force Recovery**: with power off, jumper the carrier's
   **FC_REC ↔ GND** button-header pins (or hold the recovery button), then apply
   power. Connect the dev kit to the host with **USB-C**.
3. In SDK Manager: select **Jetson Orin Nano**, **JetPack 6.2.2**, flash to the
   chosen storage (NVMe/SD), let it run to completion.

> Pitfall: SDK Manager downloads from the NVIDIA CDN can be **throttled/slow** — let
> it grind; don't kill it on a stalled bar. Method A avoids this entirely.

## 2. First boot + base config
1. Complete **oem-config** on first boot (create user `asaftg`, locale, network).
   > Pitfall: a headless/serial flash can leave **oem-config** unfinished — the box
   > has no login until you complete it. Attach a display or drive it over serial.
2. Set max performance (persist across reboots):
   ```bash
   sudo nvpmodel -m 0        # MAXN_SUPER
   sudo jetson_clocks
   ```
3. Verify the release:
   ```bash
   cat /etc/nv_tegra_release   # → R36 (release), REVISION: 4.4
   uname -r                    # → 5.15.148-tegra
   ```

## 3. Cooling — fan pinned 100% (always, including in flight)
The fan runs at a constant 100% at all times, not just on the bench. Two reasons:
guaranteed cooling under MAXN, and — on the gimbaled seeker head — a **constant fan
speed is a constant angular-momentum bias** the gimbal can ignore, whereas a
variable-speed fan spinning up/down is a torque disturbance the gimbal has to reject.
The default governor varies the speed, so we override it. From [`fan/`](fan/):
```bash
sudo ./install_fan.sh          # installs jetson-fan-max.service, masks nvfancontrol
cat /sys/class/hwmon/hwmon*/pwm1   # → 255
```

## 4. Network access (bench)
- **WiFi**: normal; whatever the dev kit joins.
- **Wired, low-latency**: the dev kit's USB-C device port exposes a host network at
  **`192.168.55.1`** — plug USB-C into a laptop for a stable link independent of WiFi
  (used for the EO live preview).

## Done
Platform is ready when: `nv_tegra_release` shows R36.4.4, `nvpmodel` is MAXN_SUPER,
`pwm1` is 255. Proceed to a sensor module (start with [`eo/`](../eo/README.md)).
