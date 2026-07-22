# Radar controls the console should expose — message for the GUI agent

**Ask: cut the radar tuning panel down to three controls.** Keep **FOV**,
**MIN SNR** and **MIN SPD**. Remove the other seven from the operator screen.

They all still work and `/ctl` keeps accepting all of them — this is about what
belongs in front of an operator, not about deleting the endpoint. The bench
still needs the full set (`radar/tools/track_replay.c` replays the corpus
through them).

## Keep

| control | `/ctl` param | why it belongs to the operator |
|---|---|---|
| **FOV ±** | `fov` | where to look. Narrowing it cuts off-axis clutter. |
| **MIN SNR** | `snrmin` | the one real sensitivity lever: detections vs false alarms. |
| **MIN SPD** | `speed` | how fast a thing must move to count as a mover. |

## Remove from the operator screen

`DEDUP` (`eps`), `MIN PTS` (`minpts`), `MERGE GATE` (`doppler`),
`CONFIRM` (`confirm`), `COAST` (`coast`), `PARK HOLD` (`park`), `EL ±` (`elmax`).

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

`EL ±` is a judgement call — its 20° default is the antenna's physical
elevation beam edge, so it is physically meaningful rather than arbitrary. It is
on the remove list because it is set-and-forget, not something to drive during a
mission. If you would rather keep it, that is defensible; the other six are not.

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
