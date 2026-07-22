#!/usr/bin/env python3
"""airec_to_jsonl.py - extract one AIREC v1 channel to JSONL (bench tool).

Each output line: "<t_pub_ns> <payload-json>". Format per recorder/docs/FORMAT.md:
segment header 64 B (magic AIRECSG1), record header 64 B
struct <4sIQQQII6I = magic REC0, crc, seq, t_src_ns, t_pub_ns, payload_len,
flags, meta[6]; payload padded to 8; flags&4 = PAD record (skip).

usage: airec_to_jsonl.py <channel-file-or-dir> [out.jsonl] [--start S] [--dur S]
"""
import json
import os
import struct
import sys

REC = struct.Struct("<4sIQQQII6I")


def records(path):
    files = [path]
    if os.path.isdir(path):
        files = sorted(
            os.path.join(path, f) for f in os.listdir(path) if not f.startswith(".")
        )
    for fp in files:
        with open(fp, "rb") as f:
            hdr = f.read(64)
            if len(hdr) < 64 or not hdr.startswith(b"AIRECSG1"):
                f.seek(0)  # tolerate a bare record stream
            while True:
                rh = f.read(64)
                if len(rh) < 64:
                    break
                magic, _crc, _seq, t_src, t_pub, plen, flags = REC.unpack(rh)[:7]
                if magic != b"REC0":
                    break
                pay = f.read(plen)
                pad = (-(64 + plen)) % 8
                if pad:
                    f.read(pad)
                if flags & 4:
                    continue
                yield t_pub, t_src, pay


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: a.split("=")[1] for a in sys.argv[1:] if "=" in a and a.startswith("--")}
    src = args[0]
    out = open(args[1], "w") if len(args) > 1 else sys.stdout
    start = float(opts.get("--start", 0))
    dur = float(opts.get("--dur", 1e18))
    t0 = None
    n = 0
    for t_pub, _t_src, pay in records(src):
        if t0 is None:
            t0 = t_pub
        rel = (t_pub - t0) / 1e9
        if rel < start:
            continue
        if rel > start + dur:
            break
        try:
            txt = pay.decode("utf-8", "strict").strip()
            json.loads(txt)
        except Exception:
            continue
        out.write(f"{t_pub} {txt}\n")
        n += 1
    print(f"wrote {n} records", file=sys.stderr)


if __name__ == "__main__":
    main()
