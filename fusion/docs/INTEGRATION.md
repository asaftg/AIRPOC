# fusion - integration contract

`fusiond` on **:8096**. Consumes the radar tracker (`:8092/stream`) and the EO
tracker (`:8095/stream`); publishes the one target picture. Start it after
both trackers (it reconnects on its own, so order only matters for how fast
the picture fills in).

## Endpoints

| endpoint | what |
|---|---|
| `GET /stream` | SSE, `data: <frame-json>\n\n` per processed input frame (~41 Hz with both feeds up); at least 1 Hz heartbeat even with both feeds down |
| `GET /stats` | one-shot JSON: health, feeds, counts, trim, knobs |
| `GET /ctl?k=v` | set knobs (see README); clamped server-side, always replies `ok` - test presence via `/stats knobs{}`, never via the 200 |

## /stream message

```json
{ "type":"fus", "rad_connected":true, "trk_connected":true,
  "frame_id":8231, "rad_frame_id":60412, "eo_frame_id":51230,
  "eo_engaged":-1, "t_out_ns":52277610647715,
  "trim":[1.10,2.20], "trim_est":[1.15,2.31,214],
  "targets":[
    { "gid":17, "src":"fus", "eo_tid":3, "rad_tid":12,
      "ang":[-0.0431,0.0012,0.0115,0.0230], "ang_src":"eo",
      "rate":[0.0260,-0.0001],
      "r_m":212.4, "rdot_mps":-3.10, "r_stale":0,
      "cls":"vehicle", "cls_conf":0.92, "conf":0.810,
      "fused_age_s":4.20, "eo_coast_s":0.00, "rad_coast_s":0.04,
      "grow":0.020, "eo_hits":46, "rad_np":61, "sus":0, "mv":1,
      "lock":{"on":true,"score":0.940} },
    { "gid":21, "src":"eo",  "eo_tid":5, "rad_tid":-1, "...":"..." },
    { "gid":22, "src":"rad", "eo_tid":-1, "rad_tid":7, "...":"..." } ] }
```

| field | meaning |
|---|---|
| `gid` | fusion's global id - stable across per-sensor tid churn and sensor restarts; never reused in a run |
| `src` | `fus` = both sensors, `eo` / `rad` = single-sensor passthrough |
| `eo_tid`, `rad_tid` | the constituent per-sensor track ids, `-1` when that side is absent |
| `ang` | az, el, width, height - **radians**, rig frame (az +right, el +up). Radar-sourced angles already include the mount trim: **do not add a trim of your own on top of fusion angles** |
| `ang_src` | `eo` = camera angles (precise), `rad` = radar angles (coarse elevation) |
| `rate` | vaz, vel in rad/s, from the angle source |
| `r_m`, `rdot_mps` | range (m) and range-rate (m/s) from the radar; `r_m:-1` = no range. `rdot` is dr/dt: **negative = closing** (note: the radar wire's per-point `v` uses the opposite sign) |
| `r_stale` | 1 = the radar side dropped; range is propagated, trust it less |
| `cls`, `cls_conf` | fusion-owned label (smoothed sticky vote over the EO class) + vote share |
| `conf` | the weaker of the two sides' confidences (staleness-decayed) |
| `fused_age_s` | seconds since this pair confirmed (0 on single-sensor rows) |
| `eo_coast_s`, `rad_coast_s` | per-side staleness in seconds, `-1` when the side is absent |
| `grow` | EO looming cue passthrough (1/s) |
| `eo_hits`, `rad_np` | per-sensor evidence counts, `-1` when absent |
| `sus`, `mv` | radar quality flags passthrough (sidelobe-suspect / mover class), `-1` when absent |
| `lock` | present only when the EO constituent is the engaged, locked track |

Absent-side numerics are `-1`; there are no nulls. Header `eo_engaged`
mirrors the EO tracker's `engaged`. Header `trim` = the mount trim (az, el,
degrees) that produced this frame's radar-sourced angles, and `trim_est` =
the observe-only estimate (az, el, degrees, sample count) - on the wire so
every recording self-documents its calibration state for offline analysis.

**The one dedup rule (why consumers need no logic):** a per-sensor tid that
appears as a constituent (`eo_tid`/`rad_tid`) of any row is never also
published as a standalone `src:"eo"`/`src:"rad"` row in the same frame.
Render `targets[]` verbatim.

**Timestamps:** `t_out_ns` is CLOCK_MONOTONIC at emit. Radar frames carry no
nanosecond stamp on their wire, so fusion timestamps them from the wire's
monotonic `timestamp` seconds; on localhost the error is far below the match
gate. (If the radar wire ever grows a `t_pub_ns`, fusion will prefer it.)

## /stats

```json
{ "version":"0.1.0",
  "feeds":{ "rad_connected":true, "trk_connected":true,
            "rad_fps":26.0, "trk_fps":15.1, "out_fps":41.0 },
  "tracks":{ "fused":2, "eo_only":1, "rad_only":3 },
  "trim":{ "az_deg":1.10, "el_deg":2.20, "source":"file",
           "est_az_deg":1.15, "est_el_deg":2.31, "est_n":214 },
  "health":{ "degraded":false, "errors":0, "last_err":"" },
  "knobs":{ "trim_az":1.10, "trim_el":2.20, "gate":1.00,
            "confirm":3, "divorce_s":0.60, "coast_s":1.00 } }
```

`trim.est_*` is the observe-only estimator (median angle residual over
healthy fused pairs; `est_n` = samples): what the trim *should* be. It never
applies itself.

## Degrade behaviour

- Radar feed down -> every row is `src:"eo"`; ranges propagate briefly, go
  `r_stale`, then disappear. `rad_connected:false`.
- EO feed down -> every row is `src:"rad"` with coarse-elevation angles.
- Both down -> heartbeat frames with `targets:[]`. Never synthetic data.
- fusiond down -> consumers fall back to the per-sensor wires; the radar ->
  gimbal EO-blind chain never touches fusion in the first place.
- Hard faults surface as `health.degraded` + nonzero process exit so a
  supervisor restart is visible.

## For the operator console (app/)

1. New `fus_client.c` mirroring `trk_client.c` (`-u 127.0.0.1:8096` on the
   app command line): consume `/stream`, re-broadcast as `/fus/stream`,
   one-shot `/fus`, `/fstats`, and forward a `fus_*` ctl namespace
   (`FUS_KEYS = {trim_az, trim_el, gate, confirm, divorce_s, coast_s}`,
   strip the prefix, token-boundary matching per the `qparam` lesson).
2. **Target list:** when the fusion feed is delivering frames, drive the list
   exclusively from `targets[]` - rows keyed `<src>:<gid>`. `fus` rows carry
   range AND class on one row (the stub at the reserved rank tier). `eo`/`rad`
   rows render as today's rows do. Fusion down -> fall back to the current
   two-source list. No client-side dedup ever - the wire guarantees it.
3. **EO overlay:** keep drawing video boxes from `:8095 /trk/stream` (60 fps
   lock rate). Build an `eo_tid -> gid` map from the latest fusion frame;
   boxes whose tid is a fused constituent get the fused symbology (ring+cross
   composite in seeker mode / thicker box in box mode, FUS badge). Radar-on-EO
   projected circles: suppress for `rad_tid`s that are fused constituents -
   the fused mark is that object's one mark. And once the list consumes
   fusion, do not apply the console's display trim to fusion angles (they are
   already rig-frame).
4. **Engage:** unchanged - a tap sends `trk_engage=<eo_tid>` from the row's
   `eo_tid` (a row with `eo_tid:-1` is radar-only: console-declared
   engagement only, as today). Fusion never proxies `/ctl` to other modules;
   lock state comes back read-only on the wire.

## For the launcher (app/launcher/)

- `start.sh`: after the trackerd block, before the console:
  ```bash
  if healthy 8096 /dev/shm/airpoc.fus_wire; then
    echo ":8096 healthy - skip"
  else
    ensure_gone "fusiond"
    launch 8096 "$BASE/fusion" ./fusiond -p 8096 -r 127.0.0.1:8092 -t 127.0.0.1:8095 && restarted=1
  fi
  ```
  (`restarted=1` so the recorder re-attaches and picks up the `fus_wire` tap.)
- `stop.sh`: add `pkill -x fusiond` to the daemon list.
- launcher `/status`: probe `port_up(8096)` as `"fus"`.
- console line: add `-u 127.0.0.1:8096`.
- systemd alternative: `fusion/systemd/airpoc-fusion.service` (+ install.sh).

## For gimbal / guidance

Consume `/stream`; fused rows are range+angle steerable. On fusion loss fall
back to the radar wire (`:8092`) directly - fusion must never be your only
path to a steerable target.
