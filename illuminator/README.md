# NIR Illuminator Module

Pulsed near-infrared (850 nm) illuminator that lights the EO scene so the camera can
run a short exposure (freeze motion) in low light. Hardware: **SG-IR850-8M** laser
illuminator with motor zoom over a 3.3 V TTL UART (9600 8N1).

- `src/` — C controller (`sg_ir850.c/.h`) + `sgctl` CLI (on/off, power, focus/zoom,
  status). Build with `make` in `src/`.
- `docs/ILLUMINATOR.md` — wiring, protocol, build, `sgctl` usage.
- `docs/NIR_SYNC.md` — syncing the illuminator pulse to the camera exposure window.

**State:** controller written; hardware integration and camera sync are open.
The exposure duty the illuminator must cover is defined by the EO AE — see
[`eo/docs/IMAGE_PIPELINE.md`](../eo/docs/IMAGE_PIPELINE.md) (duty cycle).

> This module is owned by the illuminator/sync workstream — extend the docs here.
