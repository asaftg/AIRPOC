#!/usr/bin/env python3
"""airec_from_jsonl.py - pack a refit fus wire (refit.c output) back into an
AIREC v1 channel directory the recorder's replay can serve (bench tool).

input lines: "<t_pub_ns> <frame_id> <n_fus> <n_eo> <n_rad> <json>"
usage: airec_from_jsonl.py <refit.jsonl> <out_dir> <session_t0_mono_ns>
"""
import os
import struct
import sys

CH_FUS = 8  # fus_wire ChanId (from a real segment header)


def crc32c(data):  # pure-python Castagnoli, mirrors recorder/tools/airec_dump.py
    tab = getattr(crc32c, "tab", None)
    if tab is None:
        tab = []
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
            tab.append(c)
        crc32c.tab = tab
    c = 0xFFFFFFFF
    for b in data:
        c = tab[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def main():
    src, out_dir, t0 = sys.argv[1], sys.argv[2], int(sys.argv[3])
    os.makedirs(out_dir, exist_ok=True)
    seg = open(os.path.join(out_dir, "data.00000.airec"), "wb")
    idx = open(os.path.join(out_dir, "index.bin"), "wb")
    seg.write(b"AIRECSG1" + struct.pack("<II", 1, CH_FUS) + struct.pack("<Q", t0) + b"\0" * 40)
    off = 64
    seq = 0
    for ln in open(src):
        parts = ln.split(" ", 5)
        t_pub = int(parts[0])
        meta = [int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]), 0, 0]
        pay = parts[5].rstrip("\n").encode()
        hdr = struct.pack("<4sIQQQII6I", b"REC0", crc32c(pay), seq, t_pub, t_pub,
                          len(pay), 0, *meta)
        seg.write(hdr + pay)
        pad = (-(64 + len(pay))) % 8
        if pad:
            seg.write(b"\0" * pad)
        idx.write(struct.pack("<QQIIQ", seq, t_pub, 0, off, len(pay)))
        off += 64 + len(pay) + pad
        seq += 1
    seg.close()
    idx.close()
    with open(os.path.join(out_dir, "channel.json"), "w") as f:
        f.write('{"name":"fus_wire","encoding":"json",'
                '"meta":[frame_id,n_fused,n_eo_only,n_rad_only,0,0],'
                '"source":"refit:offline","airec":1}\n')
    print(f"wrote {seq} records to {out_dir}", file=sys.stderr)


if __name__ == "__main__":
    main()
