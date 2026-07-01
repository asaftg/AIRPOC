# IMX296 ↔ NIR PCB Synchronization

Design note for time-aligning the IMX296 with a separate NIR-sensor PCB off a
shared timebase.

## The key fact: the IMX296 self-clocks

The Waveshare IMX296-130 board has its **own 54 MHz crystal**. The Jetson's
`extperiph1` camera mclk is **not** used — proven during bring-up: the sensor
only streamed once the driver's INCKSEL matched **54 MHz**, regardless of the
mclk the overlay requested.

Consequences:
- The sensor's frame timing is set by its own crystal (±tens of ppm), **not** by
  any Jetson clock. The old "37.09 vs 37.125 MHz, 0.09% error" concern is moot.
- **You cannot genlock the camera to the NIR PCB by sharing a Jetson clock** —
  the sensor ignores the Jetson mclk entirely.

## How to sync (use the sensor's hardware lines)

Two valid topologies:

1. **Camera as master** — the IMX296 `XVS` (vertical sync) / `XHS` output drives
   the NIR PCB's frame/line trigger. The PCB follows the camera.
2. **Common external trigger** — put the IMX296 in slave / external-trigger mode
   (`XTRIG`) and have the Jetson (or a shared source) trigger **both** the camera
   and the NIR PCB from one signal.

Either gives true frame-level alignment despite the independent crystals,
because both devices derive their frame start from a **common edge**, not from
matched free-running clocks.

## Driver implications

- The driver currently runs the sensor in **master free-run** (`XMSTA` via
  `0x300a`). For sync, expose slave/external-trigger mode and the `XVS/XHS/XTRIG`
  routing — do **not** hard-wire master-only.
- Sync wiring + the trigger-mode register sequence are a later phase; keep the
  driver/overlay trigger-capable now so it isn't a rewrite later.

## Optional
A 37.125/54 MHz oscillator shared at the board level (FRAMOS-style) is only
needed for genlock-grade *absolute* timing across many cameras — not for
camera↔NIR frame alignment, which the trigger path handles.
