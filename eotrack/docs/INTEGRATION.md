# eotrack - integration contract

The stable interface. Internals churn; this does not without a note here.

## Endpoints (`:8095`)

- `GET /stream` - Server-Sent Events, one `data: <json>\n\n` per message. A message is
  emitted on every detector tick, at 60 Hz for the engaged track while in track mode, and
  at least once a second as a heartbeat even when the detector is down. Emitted even when
  there are no tracks.
- `GET /stats` - health + live knobs (JSON).
- `GET /ctl?k=v&...` - set knobs; absent params keep their value; all clamped; replies `ok`.

## `/stream` message

```json
{
  "type": "trk",
  "connected": true,              // detector feed up?
  "mode": "stare",                // "stare" | "track"
  "engaged": -1,                  // engaged track id, or -1
  "frame_id": 1234,               // EO v4l2 sequence of the frame this state refers to
  "t_src_ns": 52307610549640,     // EO source timestamp - CORRELATION KEY ONLY, never diff vs wall-clock
  "t_pub_ns": 52277610549640,     // EO publish time (CLOCK_MONOTONIC, safe to diff)
  "t_out_ns": 52277610647715,     // tracker emit time (CLOCK_MONOTONIC)
  "img": { "w": 1440, "h": 1088 },
  "ifov_urad": 287.5,
  "tracks": [
    {
      "tid": 1,                   // stable per-sensor id (fusion assigns the global id)
      "state": "conf",            // "tent" | "conf" | "coast"
      "cls": "human",             // "unknown"|"human"|"vehicle"|"drone" (majority vote; fusion owns labelling)
      "cls_conf": 1.00,           // fraction of hits at the majority class
      "conf": 0.720,              // smoothed detection confidence
      "px":  [568.5, 540.0, 40.0, 80.0],     // cx, cy, w, h  (pixels, smoothed)
      "ang": [-0.0434, 0.0010, 0.0115, 0.0230], // az, el, w, h  (radians; az +right, el +up; RAW sensor frame)
      "rate": [0.0261, -0.0000],  // vaz, vel  (rad/s, position-derived)
      "s_ang": [0.0011, 0.0011],  // az, el 1-sigma position uncertainty (rad) - fusion association gate
      "grow": 0.000,              // relative angular-size growth (1/s) - looming / range closure cue
      "hits": 46,                 // lifetime measurement count (np analog)
      "age_s": 3.04,
      "coast_s": 0.00,            // seconds since the last real measurement (0 = measured this tick)
      "t_meas_ns": 52307610549640,// t_src of the last measurement (correlation key)
      "src": "app",               // dominant evidence: "app" | "mot" | "both"
      "lock": { "on": true, "score": 0.94 }  // engaged track only; absent otherwise
    }
  ]
}
```

Field parity with the radar wire (`radar/docs/INTEGRATION.md` `targets[]`): both carry a
per-sensor `tid`, position, velocity, a size/extent, a confidence, a hit count, and
staleness; fusion adds range (from radar) and the global id + label. EO has no range, so
position is angle-only; `grow` and `lock.score` are the EO-specific extras.

## `/stats`

```json
{
  "version": "0.1.0", "ifov_urad": 287.5, "img": { "w":1440, "h":1088 },
  "feed":   { "det_connected": true, "eo_tap_ok": false, "det_fps": 15.0, "out_fps": 15.0 },
  "tracks": { "live": 3, "emitted": 1 },     // live incl. latched-off; emitted = on the wire
  "lock":   { "engaged": 1, "on": true, "score": 0.94 },
  "health": { "degraded": false, "errors": 0, "last_err": "" },
  "knobs":  { "engage": 1, "gate_base": 28.0, "confirm": 3.0, "coast_s": 1.00,
              "clutter_s": 2.00, "lock": 1 }
}
```

`emitted <= live` always (emitted tracks are a subset - the clutter latch and the confirm
bar hold the rest back). `degraded` + `errors` go true and the process exits nonzero on a
hard fault (tap header mismatch, repeated publish failure) so a supervisor restart is
visible rather than a silent blind-but-green daemon.

## `/ctl` knobs

| knob | range | default | who | meaning |
|---|---|---|---|---|
| `engage` | tid, or -1 | -1 | operator | select the track to lock; -1 = none |
| `lock` | 0/1 | 1 | operator | allow the 60 fps engaged-target lock loop |
| `gate_base` | 4..200 px | 28 | bench | association gate base (scaled by measured rate) |
| `confirm` | 1..12 | 3.0 | bench | evidence score to confirm a track |
| `coast_s` | 0.2..5 s | 1.0 | bench | how long a confirmed track coasts on misses |
| `clutter_s` | 0.5..6 s | 2.0 | bench | translate-vs-oscillate horizon |

The tracker writes **no other module's** `/ctl`. Keeping the target framed (zoom, exposure,
illuminator, radar FOV) is each sensor module's own job, driven off the engaged-target wire.

## For the operator console (`app/`)

- Consume `:8095/stream` and draw its `tracks[]` as the EO boxes - this is the single box
  source; do **not** also draw the raw detector boxes on the main overlay (that is the
  duplication this module removes). Detector boxes belong on a dev/debug toggle only.
- Forward the operator's target pick as `:8095/ctl?engage=<tid>` and reflect `mode`/`engaged`
  from the wire (not the button press). Turn on the reserved LOCK styling for the engaged
  track. Namespace the knobs as `trk_*` through the proxy, matching `radar_*`/`det_*`.
- Tracker down = box layer shows NOT CONNECTED, like any dead feed.

## For fusion

Consume `:8095/stream` and `:8092/stream` and join on angle + time. EO angles are raw
sensor-frame (apply the radar<->EO mount trim on your side); use `s_ang` for the association
gate, `grow` as a range-closure cue, and `coast_s`/`t_meas_ns` for staleness. `tid` is
per-sensor and transient - assign the global id and the class label in fusion.

That module now exists: `fusion/` (`fusiond`, `:8096`) - contract in
[`fusion/docs/INTEGRATION.md`](../../fusion/docs/INTEGRATION.md).
