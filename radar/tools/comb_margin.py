#!/usr/bin/env python3
"""comb_margin.py — read the comb-gate margin telemetry out of a recording.

The radar daemon polls the chip's `queryDemoStatus` once a second and publishes
the reply on the `airpoc.radar_cli` tap, which the recorder stores as the
`radar_cli` channel. This reads that channel back and reports how detections are
distributed by empty-band margin — the number that says whether a detection is a
real echo or a DDM comb artifact.

Why it matters: past ~250 m a real target and the surrounding junk sit at the
same SNR, so nothing that sorts by strength separates them. Margin is a
different axis. Before arming the gate we need two things from real data:
  1. where the junk piles up (so the bar can go above it), and
  2. that a real target at range stays ABOVE that bar (so arming cannot delete
     people).

Usage:
    comb_margin.py <recording_dir> [--from S] [--to S]

  --from/--to select a time window in seconds from the start of the recording,
  so one movie can be split into its static part and its walking part and the
  two compared. Print both, then look at whether the walk adds counts ABOVE the
  bar (good: real targets are separable) or only below it (bad: arming would
  delete the target).
"""
import sys, os, glob, re, struct

REC_HDR = 64          # AirecRecHdr, see recorder/docs/FORMAT.md
SEG_HDR = 64          # AirecSegHdr


def records(chan_dir):
    """Yield (t_rel_s, text) for each telemetry record, oldest first."""
    t0 = None
    for path in sorted(glob.glob(os.path.join(chan_dir, "*.airec"))):
        blob = open(path, "rb").read()
        off = SEG_HDR
        while off + REC_HDR <= len(blob):
            magic, _crc, _seq, t_src, t_pub, plen, _flags = struct.unpack_from("<IIQQQII", blob, off)
            if magic != 0x30434552:               # "REC0"
                off += 1                          # resync
                continue
            body = blob[off + REC_HDR: off + REC_HDR + plen]
            if t0 is None:
                t0 = t_pub
            yield (t_pub - t0) / 1e9, body.decode("utf-8", "replace")
            off += REC_HDR + plen


BIN_RE = re.compile(r"bins\[\s*(\d+)\.\.\s*(\d+)\]:\s*((?:\d+\s*)+)")
CAND_RE = re.compile(r"lastFrame\]:\s*cand=(\d+)\s+rej=(\d+)")
MODE_RE = re.compile(r"mode=(\w+).*?\(([\d.]+) dB\).*?lsbPerDb=([\d.]+)")


def parse(chan_dir, t_from, t_to):
    hist, nsamp, cand, rej, mode = {}, 0, 0, 0, None
    for t, text in records(chan_dir):
        if t < t_from or t > t_to:
            continue
        m = MODE_RE.search(text)
        if m and mode is None:
            mode = (m.group(1), float(m.group(2)), float(m.group(3)))
        c = CAND_RE.search(text)
        if not c:
            continue                              # truncated/garbled reply
        nsamp += 1
        cand += int(c.group(1)); rej += int(c.group(2))
        for lo, _hi, vals in BIN_RE.findall(text):
            for i, v in enumerate(vals.split()):
                hist[int(lo) + i] = hist.get(int(lo) + i, 0) + int(v)
    return hist, nsamp, cand, rej, mode


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    rec = sys.argv[1]
    t_from, t_to = 0.0, 1e9
    if "--from" in sys.argv: t_from = float(sys.argv[sys.argv.index("--from") + 1])
    if "--to" in sys.argv:   t_to   = float(sys.argv[sys.argv.index("--to") + 1])

    chan = os.path.join(rec, "radar_cli")
    if not os.path.isdir(chan):
        print(f"no radar_cli channel in {rec} — recorded before the tap existed?"); sys.exit(1)

    hist, nsamp, cand, rej, mode = parse(chan, t_from, t_to)
    if not nsamp:
        print("no usable telemetry samples in that window"); sys.exit(1)

    if mode:
        print(f"gate mode={mode[0]}  bar={mode[1]:.1f} dB  lsbPerDb={mode[2]:.1f}")
    print(f"window {t_from:.0f}..{min(t_to, 1e6):.0f}s   samples={nsamp}   "
          f"detections={cand}   would-reject={rej} ({100.0*rej/max(cand,1):.0f}%)")
    print(f"{'margin dB':>12} {'count':>9}  {'share':>6}")
    tot = sum(hist.values()) or 1
    for b in sorted(hist):
        lo, hi = b * 3.01, (b + 1) * 3.01
        n = hist[b]
        bar = "#" * int(60.0 * n / max(hist.values()))
        print(f"{lo:5.0f}-{hi:<5.0f} {n:9d}  {100.0*n/tot:5.1f}%  {bar}")


if __name__ == "__main__":
    main()
