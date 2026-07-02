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

## Custom baud

3,125,000 is not a POSIX `Bxxx` constant, so `src/serial.c` sets it via
`termios2` + `BOTHER` (`ioctl(TCSETS2)`). Standard `cfsetspeed` cannot express
it.

> Pitfall: the ground bench's Python `tlv_parser.py` docstring says 921600 —
> that is stale. `radar_setup.md` (Phase C) verified **3,125,000** on hardware.
