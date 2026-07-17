# Tracker validation — the bench, the gates, the deploy checklist

## What this is

A tracker change once doubled ghosts in the field while its offline validation
stayed green. Three things let that happen: validation scored the tracker's
*internal* confirmed-track list while the wire sends a *different, filtered*
list; replays ran with knob values nobody had verified; and there was no ghost
metric at all. This bench closes all three holes. **No tracker change deploys
without a PASS from it.**

## How to run it (one command)

Build the replay tool, then point the bench at it and at the fixture
directory (the recordings, listed in [TEST_CORPUS.md](TEST_CORPUS.md)):

```
make -C radar/tools track_replay
python3 radar/tools/regression/tracker_gates.py radar/tools/track_replay \
    --fixtures <dir-with-fixture-bins>
```

Exit 0 = safe to deploy. Exit 1 = the change made something worse — the table
says exactly what and on which recording. Exit 2 = setup problem (a fixture's
bytes don't match the pinned checksum, or the baseline is stale) — the bench
refuses to run rather than produce numbers that mean nothing.

## What the gates mean

- **The bench scores only what the wire carries** (the replay's `E` lines —
  the emitted targets), never the internal track list. It also hard-fails if
  the wire ever emits a track the internal list doesn't confirm, or if the
  replay drops frames. These are the invariants that make the original
  failure impossible.
- **Every run prints the knob state it actually ran with** (the `K` header),
  so a replay log can never silently run on wrong settings.
- **Negative recordings must produce exactly zero emissions.** Empty scenes
  are empty; one ghost is a fail.
- **Everything else is "don't get worse than the frozen baseline"**:
  emissions per frame and distinct track count may not grow past a small
  margin; where a recording has a truth sidecar (what was really in the
  scene), coverage of each real mover may not drop, it may not fragment into
  more track ids, and first-emit latency may not slip more than 3 frames.
- **Ghost and mirror counts are reported per recording.** On scenes whose
  sidecar marks truth as incomplete (busy streets), ghost counts are upper
  bounds — watch the trend, not the absolute number.

After an *intentional, reviewed* behavior change, re-freeze the baseline from
the new known-good binary: same command plus `--freeze`.

## Deploy checklist (after a PASS)

1. Commit and push to main (cloud repo is the only write target).
2. On the Jetson: pull, build, restart via the launcher — **STOP then START**
   from the :8088 page. (`/start` alone can falsely report "already in
   progress": the daemons inherit the start.sh lock fd — known launcher bug.
   The STOP/START cycle is the reliable path until that is fixed.)
3. Assert tap health: `/status` must show `radar_rec:true`.
4. Sanity-check live `/stats` against what the bench predicted (frame rate,
   targets-per-frame in the same ballpark as the fixture runs).
5. **Stop here.** Recordings are OPERATOR-ONLY actions — the deploy check
   ends at the tap-health assertion. Never start a recording without the
   operator.

## Rollback

`git revert` the tracker commit(s) on main, push, then on the Jetson: pull,
build, STOP/START via the launcher, re-assert `/status` tap health. Do not
hand-edit anything on the Jetson. The baseline file stays untouched — it
still describes the tracker you just rolled back to.
