# NIR Illuminator Module

Pulsed near-infrared (850 nm) illuminator that lights the EO scene so the camera can
run a short exposure (freeze motion) in low light. Hardware: **SG-IR850-8M** laser
illuminator with motor zoom over a 3.3 V TTL UART (9600 8N1).

- `src/` — C controller (`sg_ir850.c/.h`) + `sgctl` CLI (on/off, power, focus/zoom,
  status). Build with `make` in `src/`.
- `docs/ILLUMINATOR.md` — wiring, protocol, build, `sgctl` usage.
- `docs/GUI_INTEGRATION.md` — **handoff for adding on/off/power/FOV buttons to the
  EO monitor GUI** (where/how to hook into `eo/pipeline/mjpeg.c`).
- `docs/NIR_SYNC.md` — syncing the illuminator pulse to the camera exposure window.

**State:** controller **hardware-verified on the Orin (2026-07-01)** — on/off,
power (0–255), FOV/zoom, and status queries all confirmed against the real
SG-IR850-8M (adapter CP2102 `10c4:ea60` → `/dev/ttyUSB0`). A 5 s laser-fire test
passed (device reported `laser ON / level 255 / fan auto-ON`, then clean off).
**Open:** GUI buttons (see `docs/GUI_INTEGRATION.md`) and camera sync. The exposure
duty the illuminator must cover is defined by the EO AE — see
[`eo/docs/IMAGE_PIPELINE.md`](../eo/docs/IMAGE_PIPELINE.md) (duty cycle).

> This module is owned by the illuminator/sync workstream — extend the docs here.
