# IR Illuminator — Savgood SG-IR850-8M

850 nm IR laser illuminator with a motorized zoom lens, controlled over a TTL
UART serial link. Production controller lives in `jetson/illuminator/` (C, per
`ENGINEERING_GUIDELINES.md`): a library (`libsgir850.a`) plus a CLI (`sgctl`).

Controls: laser on/off, optical power (drive current), beam FOV (zoom), and
status queries (power / level / position / fan).

## Hardware / wiring

| Line | Device | USB-TTL adapter |
|---|---|---|
| TX | pin 4 | → RX |
| RX | pin 5 | → TX |
| GND | device GND | ↔ adapter GND (common) |
| Power | 12 V V+ / GND | **separate supply — never to the adapter** |

- Logic level **3.3 V TTL**, link **9600 8N1**.
- The 12 V rail powers only the illuminator. Keep it off every signal pin and
  off the adapter/Jetson. The adapter and the 12 V supply must share GND with
  the device.
- On power-up the zoom motor self-tests and parks at max angle (widest); the
  laser stays **off** until commanded.
- **Drive level resets to max (0xFF) on every laser-on.** To run lower, send a
  `power N` immediately after `on`.

## Protocol (SG-IR850-8M)

7-byte frames, MSB-first, 9600 8N1:

```
FF  ADDR  INSTR  INSTR  DATA  DATA  SUM
B1  B2    B3     B4     B5    B6    B7
```

- `B1` sync = `0xFF`; `B2` address = `0x01` (default).
- `B7` checksum = `(B2 + B3 + B4 + B5 + B6) mod 0x100` (sync excluded).

| Action | B3 B4 B5 B6 |
|---|---|
| Laser on / off | `01 01 01/00 00` |
| Power +/- one step | `01 02 00/01 00` |
| Set power (DA 0..255) | `01 03 <lvl> 00` |
| Zoom TELE/WIDE by steps | `01 04 00/01 <steps>` |
| Zoom to position (0x0001..0x06FE) | `01 05 <hi> <lo>` |
| Zoom reset (re-home) | `01 06 00 00` |
| Query power / current / position / fan | `02 01/03/05/0F 00 00` |

Beam angle ↔ motor position is the spec's angle table (≈1.96° at pos 0x0064 →
70° at 0x06FE); `sgctl fov <deg>` interpolates it. Lower position = narrower
beam (TELE/spot), higher = wider (WIDE/flood).

## Build

```bash
cd jetson/illuminator
make                 # builds libsgir850.a + sgctl (native aarch64, no deps)
sudo make install    # installs sgctl to /usr/local/bin
```

## Usage

```bash
# port: --port DEV, else $SG_IR850_PORT, else /dev/sg-ir850
sgctl --port /dev/ttyUSB0 status      # query without emitting (safe first step)
sgctl --port /dev/ttyUSB0 on          # laser ON (starts at MAX power!)
sgctl --port /dev/ttyUSB0 power 64    # ~25% drive level
sgctl --port /dev/ttyUSB0 fov 12      # 12° beam
sgctl --port /dev/ttyUSB0 zoom-pos 1500
sgctl --port /dev/ttyUSB0 tele 20     # narrow 20 motor steps
sgctl --port /dev/ttyUSB0 off
```

### Stable device name (udev)

The USB-TTL adapter enumerates as `/dev/ttyUSB*` (order not guaranteed). Pin it
by the adapter's VID:PID so `sgctl` can default to `/dev/sg-ir850`:

```
# /etc/udev/rules.d/72-sg-ir850.rules  (fill in idVendor/idProduct from
# `udevadm info -a -n /dev/ttyUSBx`)
SUBSYSTEM=="tty", ATTRS{idVendor}=="XXXX", ATTRS{idProduct}=="YYYY", SYMLINK+="sg-ir850"
```

## Bench bring-up order (laser-safe)

1. `sgctl status` / version readback — proves framing + checksum with **no
   emission**.
2. Test the **zoom motor** (`fov`, `zoom-pos`, `reset`) — mechanical only.
3. Only then `on` at **low power** into a safe absorber, viewed on an NIR
   camera (850 nm is invisible; treat the aperture as live).

## Library API

`#include "sg_ir850.h"`, link `libsgir850.a`. See the header for the full API:
`sg_open/sg_close`, `sg_power`, `sg_set_power`, `sg_set_fov_deg`,
`sg_zoom_to_position`, `sg_zoom_step`, `sg_zoom_reset`, and the `sg_query_*`
calls. Angle helpers `sg_angle_to_position` / `sg_position_to_angle` are pure.
