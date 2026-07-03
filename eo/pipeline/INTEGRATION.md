# EO module integration (libeo) — the frozen handoff

The EO module owns the camera and the whole capture → auto-exposure → ISP datapath.
Consumers (the GUI, a recorder, a detector front-end) **do not** open `/dev/video0`,
run AE, drive the sensor, or tone-map. They link `libeo.a`, include `eo.h`, and pull
finished frames. Everything behind `eo.h` changes freely without touching consumers.

## The contract (`eo.h`, `EO_API_VERSION 1` — frozen)

```c
int    eo_start(const char *dev);   // open camera + run capture+AE+ISP. 0 ok, <0 fail
void   eo_stop(void);
int    eo_latest(const uint8_t **buf, uint64_t *seq,
                 int *w, int *h, int *stride, int *fmt);  // 1 if a frame, else 0
int    eo_connected(void);
double eo_focal_mm(void);
double eo_pixel_um(void);
```

- **`eo_latest`** hands the newest **finished, display-ready 8-bit mono** frame
  (`fmt == EO_FMT_GRAY8`), already tone-mapped + denoised at **full sensor resolution**
  (1440×1088, `stride == w`). The pointer is **stable until your next `eo_latest()`
  call on the same thread** — triple-buffered, **no copy**. `*seq` is a monotonic frame
  id; an unchanged `seq` means no new frame yet, so you can skip re-processing.
- Any out-param may be `NULL`. Returns `0` (not an error) while the camera warms up.
- Frozen: if this surface ever must change, `EO_API_VERSION` bumps and this doc says so.

### Minimal consumer

```c
#include "eo.h"
if (eo_start("/dev/video0") < 0) { /* camera busy / no sensor */ }
const uint8_t *buf; uint64_t seq, last = 0; int w, h, stride, fmt;
for (;;) {
    if (eo_latest(&buf, &seq, &w, &h, &stride, &fmt) && seq != last) {
        last = seq;
        // buf = w*h GRAY8, valid until the next eo_latest() — encode/display/detect it
    }
}
eo_stop();
```

Build: `make libeo.a`, then link `libeo.a -ljpeg -lpthread -lm` (libeo itself needs
`-lpthread -lm`; `-ljpeg` only if you also use the preview). Include `eo.h`.

## Camera ownership — single-owner V4L2

`/dev/video0` allows exactly **one** opener. So exactly one process may `eo_start()`
at a time:

- **Production:** the GUI links `libeo` and owns the camera. The EO preview
  (`eo_pipeline`) is a **bench tool** — run it only when the GUI is stopped.
- The two never run at once. `eo_start()` returns `<0` if the camera is already held.

## What's behind the surface (may change any time — do not depend on it)

Auto-exposure ("expose don't gain": lengthen exposure / drop fps before adding gain,
gain capped), the p1/p99 tone-map, the 3×3 median, sensor registers, thread layout.
Bench/preview controls (manual gain/exposure sweep, telemetry) are in `eo_bench.h`,
which is **explicitly unstable** and for the preview only — production consumers must
not include it.

## Alternative: consume the MJPEG feed instead

If you'd rather not link C at all, run the EO preview (`eo_pipeline`, which owns the
camera) and proxy its feed — same model as the radar SSE previewer:

| Endpoint (`:8091`) | Payload |
|---|---|
| `/stream` | `multipart/x-mixed-replace` MJPEG, finished frames at the selected size |
| `/stats`  | JSON telemetry (below) |
| `/ctl?...` | controls (below) |

Then the EO process owns the camera and you do zero capture/AE/ISP/encode. Pick one
model (link `libeo` **or** proxy MJPEG) — not both, because of single-owner V4L2.

## GUI control contract (`/ctl` — proxy these to the operator)

Two bandwidth levers + the full ISP panel. The **display** shrinks freely; the
**detector always runs on the full-native 1440×1088 frame**, untouched by any of this.

**Bandwidth levers**
| `/ctl` | Values | Effect |
|---|---|---|
| `?res=` | `low`(640×480) · `med`(960×720, default) · `high`(1280×960) · `native`(1440×1080) | display size — all **4:3**, so the GUI video box never changes shape |
| `?fps=` | 12…60 | **fixed** operating fps — caps exposure AND the stream rate |

**ISP panel** (everything on the previewer)
| `/ctl` | Values | |
|---|---|---|
| `?ae=` | 0/1 | auto-exposure on/off |
| `?gain=` | 0…480 | manual gain (0.1 dB/step); drops to manual |
| `?expms=` | ms | manual exposure (capped by fps) |
| `?gaincap=` | 0…480 | AE gain ceiling |
| `?median=` | 0/1 | 3×3 denoise |
| `?zoom=` | 1/2/4/8 | digital zoom |
| `?laser=`·`?power=`·`?fov=` | 0/1 · 0…255 · deg | illuminator |

**`/stats`** returns all live values so the GUI renders the current state of every knob:
`fps` (measured display rate), `sfps` (sensor rate), **`fps_cap`** (the configured
operating-fps cap = the value set via `?fps=`; the AE never changes fps, so this is the
set value, not a guess), `mean, exp_ms, duty_pct, gain, gaincap, ae, median, zoom, hfov,
vfov, sharp, res, dw, dh, connected, laser, lpower, lfov, lpresent`.

> Codec note: `/stream` is MJPEG (LAN/bench). For the RF datalink the same `res`/`fps`
> knobs apply to an H.264/RTSP output — added when the datalink is locked.
