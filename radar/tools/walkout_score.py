#!/usr/bin/env python3
"""Walk-out test scorer (bench tool).

Reads a recorded radar_wire channel (AIREC session dir, or a single .airec file)
and reports what the LIVE tracker did during the test: every confirmed track's
life, range span, and continuity — plus the headline number: the farthest range
at which a track was held.

Usage:
    python3 walkout_score.py /data/recordings/<sid>            # session dir
    python3 walkout_score.py path/to/data.0.airec              # raw channel file
    python3 walkout_score.py <A> --vs <B>                      # A/B two sessions

The wire JSON per frame: {timestamp, num_points, points:[{r,az,el,v,snr},..],
targets:[{tid,x,y,...},..]}. Targets carry x,y (m); range = hypot(x,y).
"""
import glob, json, math, os, struct, sys


def read_wire_frames(path):
    """Yield (t_rel_s, frame_dict) from an AIREC radar_wire channel."""
    if os.path.isdir(path):
        files = sorted(glob.glob(os.path.join(path, "radar_wire", "data.*.airec")))
        if not files:
            sys.exit(f"no radar_wire/data.*.airec under {path}")
    else:
        files = [path]
    t0 = None
    for fp in files:
        b = open(fp, "rb").read()
        off = 64
        while off + 64 <= len(b):
            if b[off:off + 4] != b"REC0":
                break
            ts = struct.unpack_from("<Q", b, off + 16)[0]
            plen, fl = struct.unpack_from("<II", b, off + 32)
            pay = b[off + 64:off + 64 + plen]
            off += (64 + plen + 7) & ~7
            if fl & 4:
                continue
            try:
                fr = json.loads(pay.decode("utf-8", "replace"))
            except ValueError:
                continue
            if t0 is None:
                t0 = ts
            yield (ts - t0) / 1e9, fr


def score(path):
    tracks = {}   # tid -> dict
    nframes = 0
    t_end = 0.0
    for t, fr in read_wire_frames(path):
        nframes += 1
        t_end = t
        for tg in fr.get("targets", []):
            tid = tg.get("tid")
            r = math.hypot(tg.get("x", 0.0), tg.get("y", 0.0))
            d = tracks.setdefault(tid, dict(t0=t, t1=t, rmin=r, rmax=r,
                                            frames=0, last_t=t, gaps=0))
            if t - d["last_t"] > 0.5:
                d["gaps"] += 1
            d["t1"] = t
            d["last_t"] = t
            d["rmin"] = min(d["rmin"], r)
            d["rmax"] = max(d["rmax"], r)
            d["frames"] += 1
    return dict(path=path, nframes=nframes, dur=t_end, tracks=tracks)


def report(s):
    print(f"\n=== {s['path']}  ({s['nframes']} frames, {s['dur']:.0f}s) ===")
    tracks = s["tracks"]
    if not tracks:
        print("  no confirmed tracks")
        return
    # rank by lifetime; a real walk-out target is the long-lived one
    ranked = sorted(tracks.items(), key=lambda kv: -(kv[1]["t1"] - kv[1]["t0"]))
    print(f"  confirmed tracks: {len(tracks)}")
    print("  tid    life(s)  range(m)      frames  gaps>0.5s")
    for tid, d in ranked[:12]:
        life = d["t1"] - d["t0"]
        print(f"  {tid:<6} {life:7.1f}  {d['rmin']:5.0f}-{d['rmax']:<5.0f}"
              f"  {d['frames']:6d}  {d['gaps']}")
    far = max(d["rmax"] for d in tracks.values())
    main = ranked[0][1]
    print(f"  FARTHEST track hold: {far:.0f} m")
    print(f"  main track: {main['rmin']:.0f}-{main['rmax']:.0f} m over "
          f"{main['t1']-main['t0']:.0f}s, {main['gaps']} dropouts")


def main():
    args = [a for a in sys.argv[1:] if a != "--vs"]
    if not args:
        sys.exit(__doc__)
    a = score(args[0])
    report(a)
    if "--vs" in sys.argv and len(args) > 1:
        b = score(args[1])
        report(b)
        fa = max((d["rmax"] for d in a["tracks"].values()), default=0)
        fb = max((d["rmax"] for d in b["tracks"].values()), default=0)
        print(f"\n  A/B farthest hold: {fa:.0f} m vs {fb:.0f} m "
              f"({'+' if fb >= fa else ''}{fb - fa:.0f} m)")


if __name__ == "__main__":
    main()
