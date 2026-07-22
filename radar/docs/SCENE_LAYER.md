# Scene layer — spec for the console

**For the GUI agent.** The radar module publishes a static occupancy layer for
the operator picture. This document is the contract; the radar module does not
touch `app/`.

Live on the Jetson now: `GET http://<radar>:8092/scene`.

## What it is

Radar returns that are **not moving** (`|doppler| < 0.1 m/s`), accumulated over
time into a polar grid. Over tens of seconds the world draws itself — walls,
parked cars, buildings — because a real object is in its cell nearly every
frame while a false alarm appears once in thousands.

**Display only.** It never feeds tracking, guidance or fusion. It is the
"fog of war" backdrop the targets move across.

## Endpoint

```
GET /scene                      -> current layer (JSON)
GET /scene?on=0                 -> stop accumulating (layer freezes)
GET /scene?on=1                 -> resume
GET /scene?reset=1              -> clear and start over
GET /scene?on=1&reset=1         -> both
```
Any control form also returns the current layer. **Poll at ~1 Hz** — the daemon
only refreshes the snapshot once per second, so faster polling just re-sends the
same bytes.

## Payload

```json
{ "scene": 1, "frames": 1513,
  "nr": 192, "na": 120,
  "r_step": 2.610, "az0": -60.0, "az_step": 1.00,
  "cells": [ri, ai, occ, snr,  ri, ai, occ, snr, ...] }
```

- `scene` — 1 if accumulating, 0 if paused
- `frames` — frames accumulated so far (use it to show "map age")
- `cells` — **flat** array, 4 numbers per lit cell. Only lit cells are sent
  (typically 3–5k of the 23,040, so ~50 KB).

Per cell:

| field | meaning |
|---|---|
| `ri` | range index, 0..`nr`-1 |
| `ai` | azimuth index, 0..`na`-1 |
| `occ` | **0..255 — fraction of frames this cell was occupied** |
| `snr` | **peak echo strength in dB** ever seen in this cell |

Cell centre:
```
range_m  = (ri + 0.5) * r_step            // 2.61 m bins, native to the radar
az_deg   = az0 + (ai + 0.5) * az_step     // -60 .. +60
x = range_m * sin(az_deg)    // right
y = range_m * cos(az_deg)    // forward
```

## How to draw it — the two channels mean different things

**Bind opacity to `occ`, colour to `snr`.** This is not a style choice; the two
numbers answer two different questions:

- **`occ` = is something really there.** Noise cells sit near 0; a wall reaches
  255. The dynamic range is about four orders of magnitude, so opacity alone
  separates world from noise with no threshold.
- **`snr` = can you believe *where* it is.** Bearing accuracy is SNR-limited.
  Measured wander of a cell's reported bearing over 100 s:

  | peak strength | bearing wander |
  |---|---|
  | 60 dB | ±2.6° |
  | 53 dB | ±3.4° |
  | 36–44 dB | ±14° |
  | 28 dB | **±20°** |

  So faint cells are *real returns whose angle is noise*. That is why the far
  field shows smooth arcs at constant range — the range is right, the bearing
  is wandering. Colouring by strength lets the operator see which parts of the
  picture are geometrically trustworthy instead of us hiding it.

Suggested: `turbo` or similar over 16–62 dB for colour, `alpha = occ/255`
(clamped to a small floor so faint cells stay just visible). Draw **under** the
live points and target boxes.

## What it looks like in practice

Night street recording, 100 s, by range band:

| band | lit cells | over 30% occupancy |
|---|---|---|
| 0–100 m | 1182 | 34 |
| 100–200 m | 1573 | 30 |
| 200–300 m | 1016 | 18 |
| 300–400 m | 600 | 3 |
| 400–500 m | 505 | 1 |

Solid structure inside ~150 m, thinning to a faint haze past 300 m. The haze is
the bearing-noise population — deliberately not filtered out, because with the
sensor moving it is expected to firm up.

## Caveats the console should respect

1. **Accumulated in the SENSOR frame.** If the sensor slews, the layer smears.
   Until the gimbal encoders let us accumulate in a world frame, the console
   should `reset=1` on a slew (or offer the operator a "clear map" control).
2. **It is not a detector.** Do not derive targets, tracks or alerts from it.
3. **A moving object leaves no mark** — it is in each cell for a second or two
   out of hundreds, so it stays near `occ`≈0. Fine, and intended.
4. **`frames` is the map's age.** Useful to grey out or warn when a reset has
   just happened and the map is still forming (below ~250 frames ≈ 10 s).

## Cost

Grid is 192×120 = 23,040 cells, ~120 KB resident. Accumulation is one pass over
the ~50 stationary points per frame — microseconds, against the 446 µs the slow
detector already costs. Snapshot serialisation happens once per second.
