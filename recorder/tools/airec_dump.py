#!/usr/bin/env python3
"""airec_dump — inspect/verify an AIREC v1 session (bench tool, not shipped).

  airec_dump.py <session_dir> [--verify] [--chan NAME]

Prints per-channel record counts, time spans, gap flags; --verify walks every
record and checks magic + CRC32C of every payload against the header.
"""
import argparse, os, struct, sys, zlib

SEG_HDR = struct.Struct("<QII Q I 36x")            # magic,ver,chan,t0,segno
REC_HDR = struct.Struct("<II QQQ II 6I")           # magic,crc,seq,tsrc,tpub,len,flags,meta[6]
IDX_ROW = struct.Struct("<QQ IIII")                # seq,tsrc,seg,off,len,flags
SEG_MAGIC = 0x3147534345524941
REC_MAGIC = 0x30434552

def crc32c(data):
    try:
        import crc32c as _c
        return _c.crc32c(data)
    except ImportError:
        pass
    # slow fallback
    poly = 0x82F63B78
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (poly if c & 1 else 0)
        tab.append(c)
    c = 0xFFFFFFFF
    for b in data:
        c = tab[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF

def check_channel(cdir, name, verify):
    ipath = os.path.join(cdir, "index.bin")
    if not os.path.exists(ipath):
        return
    rows = []
    with open(ipath, "rb") as f:
        while True:
            b = f.read(IDX_ROW.size)
            if len(b) < IDX_ROW.size:
                break
            rows.append(IDX_ROW.unpack(b))
    segs = {}
    for s in range(100000):
        p = os.path.join(cdir, f"data.{s:05d}.airec")
        if not os.path.exists(p):
            break
        segs[s] = p
    gaps = sum(1 for r in rows if r[5] & 0x2)
    span = (rows[-1][1] - rows[0][1]) / 1e9 if len(rows) > 1 else 0.0
    print(f"{name:12s} records={len(rows):8d} segs={len(segs)} span={span:8.1f}s gap_flags={gaps}")

    bad = 0
    if verify:
        fh = {s: open(p, "rb") for s, p in segs.items()}
        for seq, tsrc, seg, off, plen, flags in rows:
            f = fh.get(seg)
            if not f:
                bad += 1
                continue
            f.seek(off)
            hb = f.read(REC_HDR.size)
            if len(hb) < REC_HDR.size:
                bad += 1
                continue
            h = REC_HDR.unpack(hb)
            if h[0] != REC_MAGIC or h[2] != seq or h[5] != plen:
                bad += 1
                continue
            if crc32c(f.read(plen)) != h[1]:
                bad += 1
        for f in fh.values():
            f.close()
        print(f"{'':12s} verify: {len(rows)-bad}/{len(rows)} OK" + ("  <-- CORRUPT" if bad else ""))
    return bad if verify else 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--chan")
    a = ap.parse_args()

    mf = os.path.join(a.session, "manifest.json")
    if os.path.exists(mf):
        import json
        m = json.load(open(mf))
        print(f"session {m.get('sid')} state={m.get('state')} name={m.get('name')!r} "
              f"dur={m.get('dur_ms', 0)/1000:.1f}s drops={m.get('totals', {}).get('drops')}")
    total_bad = 0
    for name in sorted(os.listdir(a.session)):
        cdir = os.path.join(a.session, name)
        if os.path.isdir(cdir) and (not a.chan or a.chan == name) and name != "thumbs":
            total_bad += check_channel(cdir, name, a.verify) or 0
    sys.exit(1 if total_bad else 0)

if __name__ == "__main__":
    main()
