# NIR Illuminator Module

Near-infrared (850 nm) illuminator that lights the EO scene so the camera can run
a short exposure (freeze motion) in low light. Hardware: **SG-IR850-8M**
(Savgood 850 nm 8 m IR flashlight) — laser illuminator with motor zoom over a
3.3 V TTL UART (9600 8N1).

> **Continuous-on, not pulsed.** The fitted device has no strobe or trigger
> command in its protocol. Pulsing the illuminator in step with the camera
> exposure window needs a custom illuminator and is not planned for this unit.

- `src/` — C controller (`sg_ir850.c/.h`) + `sgctl` CLI (on/off, power, focus/zoom,
  status). Build with `make` in `src/`.
- `docs/ILLUMINATOR.md` — wiring, protocol, build, `sgctl` usage.
- `docs/PREVIEW_INTEGRATION.md` — how the on/off/power/FOV controls were wired into the
  EO monitor (**✅ implemented** in `eo/pipeline/` — `illum.c` + `mjpeg.c`).
- `docs/NIR_SYNC.md` — future camera/illuminator sync. Note it covers a separate
  NIR sensor board, not this flashlight.

**State:** controller **hardware-verified on the Orin (2026-07-01)** — on/off,
power (0–255), FOV/zoom, and status queries all confirmed against the real
SG-IR850-8M (adapter CP2102 `10c4:ea60` → `/dev/ttyUSB0`). A 5 s laser-fire test
passed (device reported `laser ON / level 255 / fan auto-ON`, then clean off).
On/off, power, and beam-FOV controls are **live in the EO preview**
(`eo/pipeline/`, verified on-device). **Open:** no udev rule is shipped, so the
EO pipeline talks to `/dev/ttyUSB0` — the first USB-serial adapter to enumerate
(`docs/ILLUMINATOR.md`); and commands are fire-and-forget, the device's own
state is never read back. The exposure duty the illuminator must cover
is defined by the EO AE — see
[`eo/docs/IMAGE_PIPELINE.md`](../eo/docs/IMAGE_PIPELINE.md) (duty cycle).

> This module is owned by the illuminator/sync workstream — extend the docs here.
