# NIR Illuminator Module

Pulsed near-infrared (850 nm) illuminator that lights the EO scene so the camera can
run a short exposure (freeze motion) in low light. Hardware: **SG-IR850-8M** laser
illuminator with motor zoom over a 3.3 V TTL UART (9600 8N1).

- `src/` — C controller (`sg_ir850.c/.h`) + `sgctl` CLI (on/off, power, focus/zoom,
  status). Build with `make` in `src/`.
- `docs/ILLUMINATOR.md` — wiring, protocol, build, `sgctl` usage.
- `docs/PREVIEW_INTEGRATION.md` — how the on/off/power/FOV controls were wired into the
  EO monitor (**✅ implemented** in `eo/pipeline/` — `illum.c` + `mjpeg.c`).
- `docs/NIR_SYNC.md` — syncing the illuminator pulse to the camera exposure window.

**State:** controller **hardware-verified on the Orin (2026-07-01)** — on/off,
power (0–255), FOV/zoom, and status queries all confirmed against the real
SG-IR850-8M (adapter CP2102 `10c4:ea60` → `/dev/ttyUSB0`). A 5 s laser-fire test
passed (device reported `laser ON / level 255 / fan auto-ON`, then clean off).
On/off, power, and beam-FOV controls are **live in the EO preview**
(`eo/pipeline/`, verified on-device). **Open:** syncing the pulse to the camera
exposure window (`docs/NIR_SYNC.md`). The exposure duty the illuminator must cover
is defined by the EO AE — see
[`eo/docs/IMAGE_PIPELINE.md`](../eo/docs/IMAGE_PIPELINE.md) (duty cycle).

> This module is owned by the illuminator/sync workstream — extend the docs here.
