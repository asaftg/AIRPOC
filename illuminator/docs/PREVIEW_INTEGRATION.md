# Illuminator — Preview Integration Guide (handoff)

> **✅ IMPLEMENTED (2026-07-01).** The on/off, power, and beam-FOV controls are live
> in the preview (`eo/pipeline/`): shim `eo/pipeline/illum.c`/`illum.h`,
> buttons + `/ctl?laser=/power=/fov=` + `/stats` fields in `eo/pipeline/mjpeg.c`,
> opened at startup in `main.c` (`-i /dev/ttyUSB0`, optional). Built and **verified
> on-device** — illuminator detected, FOV/power/on-off controllable. This document is
> now the *reference* for how it was wired (below); no re-integration needed.

This document was the handoff for adding **on/off, power, and FOV** controls for the
Savgood **SG-IR850-8M** IR illuminator to the EO preview (the EO pipeline's
monitor). It records the hardware-verified controller and exactly how the buttons
hook in. Read alongside [`ILLUMINATOR.md`](ILLUMINATOR.md) (wiring + full protocol).

---

## 1. Status — what already works (hardware-verified on the Orin, 2026-07-01)

The controller is **done and 100% verified on the real device** (laser off except a
deliberate 5 s fire test):

| Capability | Verified | How |
|---|---|---|
| Serial link + wiring | ✅ | `sgctl status` returns valid checksummed frames |
| Laser on/off | ✅ | 5 s fire: status read `laser ON, level 255, fan auto-ON` → then off |
| Power (drive current 0–255) | ✅ | reported 255 during fire, 0 after |
| FOV / zoom | ✅ | motor moved to 20°→916, 12°→612, 70°→1790, read back exactly |
| Status queries | ✅ | power / level / position / fan all decode correctly |

Nothing about the controller needs changing. **The preview work is purely: call the
existing control API from the web server and add UI widgets.**

### Where it lives
```
illuminator/src/
  sg_ir850.h     public API (open, power, set_power, set_fov_deg, zoom, queries)
  sg_ir850.c     protocol + termios serial (9600 8N1), no external deps
  sgctl.c        CLI (reference for how the API is called)
  Makefile       builds libsgir850.a + sgctl
```
On the Jetson: adapter is a **CP2102 (`10c4:ea60`) on `/dev/ttyUSB0`**; user
`asaftg` is in `dialout`, so no sudo is needed to open the port.

---

## 2. The preview it plugs into

The EO preview is the **EO pipeline's MJPEG monitor server**, in C:
`eo/pipeline/mjpeg.c`. It already implements the exact pattern the illuminator
buttons need:

- Serves an HTML page (`PAGE`) with a control bar of buttons (currently digital
  zoom `1x/2x/4x/8x` + focus).
- A **`GET /ctl?...`** endpoint that parses query params and mutates state
  (`/ctl?zoom=4`).
- A **`GET /stats`** endpoint returning JSON, polled by the page every 150 ms to
  update the overlay.

So illuminator controls are the same three edits: **add widgets to `PAGE`**, **add
cases to the `/ctl` handler**, **add fields to `/stats`**. Plus opening the device
once at startup and linking the library.

> The server runs the accept loop on one thread and each request on its own client
> thread (`pthread_create` per connection in `mjpeg.c`). So `/ctl` handlers run off
> the capture loop — blocking serial writes there are fine (see gotchas).

---

## 3. Control API (call this from the server)

`#include "sg_ir850.h"`, link `libsgir850.a`. The handle is a small struct holding
the open fd; open it once and reuse it.

```c
sg_ir850_t dev;
sg_open(&dev, "/dev/ttyUSB0", 0);         // 0 = default addr 0x01, 9600 8N1

sg_power(&dev, true);                      // laser ON  (see gotcha: resets level!)
sg_power(&dev, false);                     // laser OFF
sg_set_power(&dev, level_0_255);           // optical drive level
sg_set_fov_deg(&dev, degrees);             // 1.96 .. 70  (angle-table interpolation)
sg_zoom_to_position(&dev, pos_1_1790);     // raw motor pos if you prefer
sg_query_power(&dev, &on);                 // 0/1
sg_query_current(&dev, &level);            // 0..255
sg_query_position(&dev, &pos);             // 1..1790  (sg_position_to_angle(pos)->deg)
sg_query_fan(&dev, &on);
```

Ranges the UI must respect:
- **power**: `0..255` (map a 0–100 % slider: `level = round(pct*255/100)`).
- **FOV**: `1.96 .. 70` degrees. Lower ° = narrower/spot, higher ° = wider/flood.
- Motor position (if used directly): `1 .. 1790` (`1`=spot, `1790`=flood).

---

## 4. Recommended integration (production-correct)

Link the library into `eo_pipeline` and drive it in-process. Do **not** shell out to
`sgctl` per click (fork/exec + reopen the port each time is churn, not production
clean). A subprocess call is only an acceptable throwaway if the preview is a separate
process from the C pipeline.

### 4a. A tiny thread-safe shim
Multiple `/ctl` clients can arrive concurrently, so guard the handle with a mutex.
Keep a `desired_power` so the on/off + power semantics are intuitive (see gotcha).
Suggested new file `eo/pipeline/illum.c` (+ `illum.h`), or inline in `mjpeg.c`:

```c
static pthread_mutex_t il_lock = PTHREAD_MUTEX_INITIALIZER;
static sg_ir850_t il_dev;               // fd = -1 until opened
static int il_ok = 0;                   // device present?
static int il_on = 0, il_power = 64;    // cached commanded state (power default ~25%)
static double il_fov = 70.0;

int  illum_start(const char *port);     // sg_open; set il_ok; call once at startup
void illum_set_on(int on);              // ON: sg_power(true) THEN sg_set_power(il_power)
void illum_set_power(int level);        // clamp 0..255; if il_on, apply now
void illum_set_fov(double deg);         // clamp 1.96..70; sg_set_fov_deg
void illum_snapshot(int *on,int *pw,double *fov,int *present); // for /stats
```
Each setter takes `il_lock`, calls the `sg_*` function, updates the cached state.
If `!il_ok`, setters no-op (the pipeline must run fine with no illuminator attached).

### 4b. Open it at startup (`main.c`)
Add an option and open once (optional device — failure is non-fatal):
```c
// getopt: add 'i': const char *iport = "/dev/ttyUSB0"; case 'i': iport = optarg;
illum_start(iport);     // logs a warning and continues if not present
```

### 4c. Extend the `/ctl` handler (`mjpeg.c`, in `client()`)
Alongside the existing `zoom=` parse:
```c
if (strncmp(req, "GET /ctl", 8) == 0) {
    char *q;
    if ((q = strstr(req, "zoom=")))   { int v=atoi(q+5); if(v==1||v==2||v==4||v==8) g_zoom=v; }
    if ((q = strstr(req, "laser=")))  illum_set_on(atoi(q+6));      // 0/1
    if ((q = strstr(req, "power=")))  illum_set_power(atoi(q+6));   // 0..255
    if ((q = strstr(req, "fov=")))    illum_set_fov(atof(q+4));     // degrees
    /* ...existing "ok" response... */
}
```

### 4d. Add the widgets to `PAGE` (`mjpeg.c`)
Add to the `#bar` control div and the `<script>`:
```html
&nbsp;&nbsp;LASER
<button onclick="L(1)" id=lon>ON</button><button onclick="L(0)" id=loff>OFF</button>
pow <input type=range min=0 max=255 value=64 id=pw oninput="P(this.value)">
fov <input type=range min=2 max=70 value=70 id=fv oninput="F(this.value)">
```
```js
function L(v){fetch('/ctl?laser='+v)}
function P(v){fetch('/ctl?power='+v)}
function F(v){fetch('/ctl?fov='+v)}
```
Drive the button highlight and slider values from `/stats` in the existing `t()`
poll so the UI reflects real state (and a bright **“LASER ON”** indicator — this is
a live-fire control).

### 4e. Add illuminator state to `/stats` (`mjpeg.c`)
Extend the JSON so the page can reflect actual state:
```c
int lon, lpw, lpr; double lfov;
illum_snapshot(&lon, &lpw, &lfov, &lpr);
// ...append to body: "laser":lon, "lpower":lpw, "lfov":lfov, "lpresent":lpr
```
Use the **cached** commanded state here — do **not** issue serial queries on every
150 ms `/stats` poll (that would flood the 9600-baud link). Optionally reconcile
with a real `sg_query_*` at a low rate (e.g. once/sec) in a background tick.

### 4f. Makefile (`eo/pipeline/Makefile`)
Link the controller. Either build the object directly or reference the sibling lib:
```make
# add sg_ir850.o (compiled from ../../illuminator/src) + illum.o to the link,
# and -I../../illuminator/src for the header.
```

---

## 5. Gotchas (read before implementing)

1. **Power resets to MAX (0xFF) on every laser-on.** The device forces full drive
   whenever it turns on. So “ON” must be `sg_power(true)` **immediately followed by**
   `sg_set_power(desired_power)`, or the operator’s slider won’t match reality. The
   shim’s `illum_set_on()` does this. Never assume the level persists across an
   off→on cycle.
2. **Serial writes block** (~a few ms each; the driver does `tcdrain` + a 2 ms
   inter-command gap). Fine on the per-request `/ctl` threads. **Never call `sg_*`
   from the capture loop in `main.c`** — it would stall frames.
3. **Thread-safety.** `/ctl` runs on concurrent client threads; guard the single
   `sg_ir850_t` with a mutex (the shim does).
4. **Beam FOV ≠ camera FOV.** `/stats` already reports a `hfov/vfov` for the
   *camera’s* digital zoom. The illuminator FOV is the *light beam* angle — a
   different thing. Label the widget clearly (e.g. “beam”) so the operator isn’t
   confused. (Later, the beam should track the camera FOV — that’s a NIR-sync
   feature, see [`NIR_SYNC.md`](NIR_SYNC.md), out of scope for buttons.)
5. **Eye safety / live-fire.** 850 nm is an invisible laser (~800 m class). The
   “ON” button emits immediately at full power. Give it a prominent on-state
   indicator; consider a confirm. This is not a cosmetic toggle.
6. **Port stability.** `/dev/ttyUSB0` is enumeration-order and not guaranteed across
   reboots/replugs. Recommend a udev symlink so the pipeline can default to a stable
   name (VID:PID is `10c4:ea60`):
   ```
   # /etc/udev/rules.d/72-sg-ir850.rules
   SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", SYMLINK+="sg-ir850"
   ```
   (CP2102 shares this VID:PID across units; if another CP2102 is ever attached,
   disambiguate by `ATTRS{serial}`.) This udev rule + an installer belong in the
   repo (commit→push→pull), not a hand-edit on the Jetson.
7. **Optional device.** If the illuminator isn’t attached, `illum_start` fails
   gracefully and all setters no-op — the EO pipeline must keep running.

---

## 6. Protocol reference (for completeness)

7-byte frames, 9600 8N1, `SUM = (B2+B3+B4+B5+B6) mod 0x100`:

| Action | B3 B4 B5 B6 |
|---|---|
| Laser on / off | `01 01 01/00 00` |
| Set power (0..255) | `01 03 <lvl> 00` |
| Zoom to position (1..1790) | `01 05 <hi> <lo>` |
| Zoom TELE/WIDE by steps | `01 04 00/01 <steps>` |
| Zoom reset (re-home) | `01 06 00 00` |
| Query power/current/position/fan | `02 01/03/05/0F 00 00` |

All of this is already implemented in `sg_ir850.c` — the preview never touches raw
frames, it calls the API in §3. Full protocol + the beam-angle table:
[`ILLUMINATOR.md`](ILLUMINATOR.md); source spec: Savgood “SG-IR850-8M Communication
Protocols” PDF.
