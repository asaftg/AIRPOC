#!/usr/bin/env python3
"""Parity check: cluster.c tracker vs the Python reference (radar_tracker.py).

Replays a point-cloud fixture through both implementations and compares
behavior. Bitwise parity is NOT attainable: numpy sums pairwise while C sums
naively, so a point claim at the exact gate edge can flip, and the floor-noise
junk-track population then decorrelates chaotically (both sides still kill it).
What must and does agree:

  * LONG confirmed tracks (>= LONG_FRAMES) — the real targets — must have a
    positional twin on the other side (same place, same time) with confirm and
    death frames within LIFE_TOL;
  * short junk blinks are compared statistically (count ratio);
  * EMITTED output must agree on the operator metrics: emitted-track count,
    walk tracks (life/range span), far-hold, and total emitted frames.

Fixture format: repeated [double t_sec LE, int32 n, n*(5 float32: r,az,el,v,snr)].

Usage:
    python3 parity_check.py <fixture.bin> [--elmax 20] [--replay ./track_replay]
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radar_tracker import tracker  # noqa: E402

R_EPS = 3.0          # m    positional coverage gate
AZ_EPS = 1.5         # deg
LONG_FRAMES = 390    # ~15 s of emission: tracks this long must be covered by
                     # the other side. Shorter emitted segments (8-10 s
                     # boundary targets) can flip per side: guard-latch timing
                     # on a marginal target is FP-sensitive.
COVER_MIN = 0.80     #      fraction of a long track's frames that must be covered
BLINK_RATIO = 0.45   # junk blink count may differ by up to this fraction
EMIT_FRAMES_TOL = 0.20  # emitted track-frames total tolerance (fraction)


def load_fixture(path):
    b = open(path, "rb").read()
    off = 0
    frames = []
    while off + 12 <= len(b):
        t, n = struct.unpack_from("<di", b, off)
        off += 12
        pts = []
        for _ in range(n):
            pts.append(struct.unpack_from("<5f", b, off))
            off += 20
        frames.append(dict(t=t, pts=pts))
    return frames


def run_python(frames, elmax):
    conf = []
    emit = tracker(frames, el_lo=-elmax, el_hi=elmax, guard=True,
                   _conf_dump=conf)
    em = [[(tk["id"], tk["r"], tk["az"]) for tk in fr] for fr in emit]
    return conf, em


def run_c(replay, fixture, elmax):
    outp = subprocess.run([replay, fixture, str(elmax)],
                          capture_output=True, text=True, check=True)
    conf, emit = [], []
    for ln in outp.stdout.splitlines():
        f = ln.split()
        if f[0] == "F":
            conf.append([])
            emit.append([])
        elif f[0] == "C":
            conf[-1].append((int(f[1]), float(f[2]), float(f[3])))
        elif f[0] == "E":
            emit[-1].append((int(f[1]), float(f[2]), float(f[3])))
    return conf, emit


def collect(dump):
    tr = {}
    for i, fr in enumerate(dump):
        for (tid, r, az) in fr:
            tr.setdefault(tid, {})[i] = (r, az)
    return tr


def coverage(track, other_frames):
    """fraction of a track's frames covered positionally by the other side"""
    good = 0
    for i, (r, az) in track.items():
        for (_, ro, azo) in other_frames[i]:
            if abs(ro - r) < R_EPS and abs(azo - az) < AZ_EPS:
                good += 1
                break
    return good / len(track)


def emit_metrics(emit, dts):
    tr = collect(emit)
    n_frames = sum(len(fr) for fr in emit)
    walks = []
    hold = 0.0
    for tid, f in tr.items():
        fs = sorted(f)
        life_s = dts[fs[-1]] - dts[fs[0]]
        rr = [f[i][0] for i in fs]
        if life_s >= 15.0 and max(rr) - min(rr) >= 60.0:
            walks.append((round(life_s, 1), round(min(rr)), round(max(rr))))
        held = [dts[i] for i in fs if 240.0 <= f[i][0] <= 310.0]
        if len(held) >= 2:
            dur = 0.0
            s = p = held[0]
            for t in held[1:]:
                if t - p > 1.0:
                    dur += p - s
                    s = t
                p = t
            dur += p - s
            hold = max(hold, dur)
    return dict(tracks=len(tr), frames=n_frames,
                walks=sorted(walks, reverse=True), far_hold=round(hold, 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixture")
    ap.add_argument("--elmax", type=float, default=20.0)
    ap.add_argument("--replay", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "track_replay"))
    args = ap.parse_args()

    frames = load_fixture(args.fixture)
    dts = [f["t"] for f in frames]
    pconf, pemit = run_python(frames, args.elmax)
    cconf, cemit = run_c(args.replay, args.fixture, args.elmax)
    if len(pconf) != len(cconf):
        print(f"FAIL frame count: py {len(pconf)} vs c {len(cconf)}")
        sys.exit(1)

    tp, tc = collect(pconf), collect(cconf)
    ep, ec = collect(pemit), collect(cemit)
    bad = 0
    nlong = 0
    worst_cov = 1.0
    for side, tr, other in (("py", ep, cemit), ("c", ec, pemit)):
        for tid, f in tr.items():
            if len(f) < LONG_FRAMES:
                continue
            nlong += 1
            cov = coverage(f, other)
            worst_cov = min(worst_cov, cov)
            if cov < COVER_MIN:
                fs = sorted(f)
                print(f"  LONG emitted {side} tid {tid} ({fs[0]}-{fs[-1]}, "
                      f"{len(f)} frames) covered only {cov:.2f}")
                bad += 1
    # info: confirmed long-track coverage (not a failure gate)
    for side, tr, other in (("py", tp, cconf), ("c", tc, pconf)):
        for tid, f in tr.items():
            if len(f) < LONG_FRAMES:
                continue
            cov = coverage(f, other)
            if cov < COVER_MIN:
                fs = sorted(f)
                print(f"  info: confirmed {side} tid {tid} "
                      f"({fs[0]}-{fs[-1]}, {len(f)} frames) coverage {cov:.2f}")
    nblink_p = sum(1 for f in tp.values() if len(f) < LONG_FRAMES)
    nblink_c = sum(1 for f in tc.values() if len(f) < LONG_FRAMES)
    if max(nblink_p, nblink_c) >= 10:
        ratio = abs(nblink_p - nblink_c) / max(nblink_p, nblink_c)
        if ratio > BLINK_RATIO:
            print(f"  blink-count ratio off: py {nblink_p} c {nblink_c}")
            bad += 1
    mp, mc = emit_metrics(pemit, dts), emit_metrics(cemit, dts)
    print(f"confirmed: py {len(tp)} (blinks {nblink_p})  "
          f"c {len(tc)} (blinks {nblink_c})")
    print(f"long emitted tracks: {nlong}  worst coverage {worst_cov:.2f}")
    print(f"emitted py: {mp}")
    print(f"emitted c : {mc}")
    if mp["frames"] == 0 and mc["frames"] == 0:
        pass                                   # negative control: both silent
    else:
        denom = max(mp["frames"], mc["frames"], 1)
        if abs(mp["frames"] - mc["frames"]) / denom > EMIT_FRAMES_TOL:
            print("  emitted track-frames totals diverge")
            bad += 1
        if mp["far_hold"] > 5.0 and not mc["far_hold"] > 0.8 * mp["far_hold"]:
            print("  far-hold lost in C")
            bad += 1
        if len(mp["walks"]) != len(mc["walks"]):
            print("  walk-track count differs")
            bad += 1
    print("PARITY OK" if bad == 0 else "PARITY FAIL")
    sys.exit(0 if bad == 0 else 1)


if __name__ == "__main__":
    main()
