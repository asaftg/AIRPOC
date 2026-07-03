# Radar hardware — AWR2944PEVM

- **Board:** TI AWR2944PEVM, 77 GHz FMCW, 4TX / 4RX. **No DCA1000** — the
  raw-ADC/LVDS path does not exist in AIRPOC; all data is the UART TLV cloud.
- **USB:** the on-board XDS110 enumerates **two** CDC-ACM serial ports:
  - **CLI** — 115200 baud. Profile push, `sensorStart`/`sensorStop`.
  - **Data** — **3,125,000 baud**. The TLV point-cloud stream.

## udev symlinks (stable names)

The two ACM nodes enumerate in an unstable order. Pin them by the XDS110
interface number so `/dev/radar-cli` and `/dev/radar-data` are deterministic,
e.g. `/etc/udev/rules.d/70-radar.rules`:
```
SUBSYSTEM=="tty", ATTRS{idVendor}=="0451", ATTRS{idProduct}=="bef3", \
  ENV{ID_USB_INTERFACE_NUM}=="00", SYMLINK+="radar-cli"
SUBSYSTEM=="tty", ATTRS{idVendor}=="0451", ATTRS{idProduct}=="bef3", \
  ENV{ID_USB_INTERFACE_NUM}=="03", SYMLINK+="radar-data"
```
> Pitfall: confirm the vendor/product and which interface number is CLI vs
> data **on the actual board** (`udevadm info -a /dev/ttyACM*`) before trusting
> the mapping — the numbers above are the expected XDS110 layout, not verified
> on this unit. The daemon takes `-C`/`-D` overrides meanwhile.

## ModemManager MUST be off (verified 2026-07-03)

Linux **ModemManager auto-probes every new CDC-ACM device** as if it were a
cellular modem. When the XDS110 ports re-enumerate on a radar power-cycle, it
grabs `/dev/ttyACM0/1` and injects AT commands **right during the daemon's cfg
push** — early cfg lines are lost, `sensorStart` runs half-applied, and the
sensor produces **no frames** (and you see `Device or resource busy`). Disable
it on the Jetson:
```
sudo systemctl stop ModemManager && sudo systemctl mask ModemManager
```
(Belt-and-suspenders: a udev `ENV{ID_MM_DEVICE_IGNORE}="1"` rule for vendor
`0451` also works, but masking is simplest on a dedicated seeker.) The daemon
also now **waits for the CLI prompt** before pushing (`cfg_push.c`), so it never
pushes into a still-booting chip.

## Custom baud

3,125,000 is not a POSIX `Bxxx` constant, so `src/serial.c` sets it via
`termios2` + `BOTHER` (`ioctl(TCSETS2)`). Standard `cfsetspeed` cannot express
it.

> Pitfall: the ground bench's Python `tlv_parser.py` docstring says 921600 —
> that is stale. `radar_setup.md` (Phase C) verified **3,125,000** on hardware.
