# Radar controls the console should expose — message for the GUI agent

**Ask: cut the radar tuning panel down to four controls.** Keep **FOV**,
**EL**, **MIN SNR** and **MIN SPD**. Remove the other six from the operator
screen.

They all still work and `/ctl` keeps accepting all of them — this is about what
belongs in front of an operator, not about deleting the endpoint. The bench
still needs the full set (`radar/tools/track_replay.c` replays the corpus
through them).

## Keep

| control | `/ctl` param | why it belongs to the operator |
|---|---|---|
| **FOV ±** | `fov` | where to look, horizontally. Narrowing it cuts off-axis clutter. |
| **EL ±** | `elmax` | where to look, vertically. Same control, other axis: narrow it to reject ground clutter and multipath from below. Clamps 5-90 deg, exactly like FOV. Default 20 deg is the antenna's beam edge - the useful CEILING, not a fixed setting. |
| **MIN SNR** | `snrmin` | the one real sensitivity lever: detections vs false alarms. |
| **MIN SPD** | `speed` | how fast a thing must move to count as a mover. |

## Remove from the operator screen

`DEDUP` (`eps`), `MIN PTS` (`minpts`), `MERGE GATE` (`doppler`),
`CONFIRM` (`confirm`), `COAST` (`coast`), `PARK HOLD` (`park`).

Two reasons:

1. **They are tracker internals.** Nobody can reason about "merge gate 1.2 m/s"
   in the field. They were exposed for development tuning.
2. **They are part of a validated configuration.** The defaults are the
   offline-validated operating point, and
   `radar/tools/regression/tracker_baseline.json` pins them as a `knob_vector`:
   `eps=4.500 minpts=2 speed=0.700 snrmin=16.000 fov=90.000 elmax=20.000
   doppler=1.200 confirm=3 coast=0.400 park=15.000`.
   Moving one in the field silently invalidates every corpus result, with no
   record of what changed.

## MIN SNR and MIN SPD now drive BOTH detectors

The radar module runs two detectors merged into one target list (see
[`SLOWDET.md`](SLOWDET.md)): `cluster.c` confirms per frame, `slowdet.c` chains
faint intermittent echoes across frames. **Treat them as one tracker.** As of
this change, `snrmin` and `speed` are applied to both, so one pair of controls
governs the whole thing. Nothing extra to add to the UI, and **no S on/off** —
there is deliberately no separate switch.

One internal is intentionally NOT bound to `speed`: the slow detector's
Doppler-liveness gate, used to decide whether an echo may extend a chain. A
walking human reads below 0.5 m/s in ~62% of frames, so pushing the operator's
MIN SPD onto it would sever the chain and delete exactly the far targets that
detector exists to catch. `MIN SPD` correctly maps to the *position-derived*
travel requirement instead.

## While you are in there

The module also publishes a **scene layer** (static occupancy backdrop) at
`GET /scene`, with its own show/hide and clear. Spec:
[`SCENE_LAYER.md`](SCENE_LAYER.md). Independent of the controls above.
