#!/usr/bin/env python3
"""verify_replay_match — prove native replay renders what the operator saw.

For frames where the operator's display was at native resolution, compare the
recorder's NATIVE replay frame (raw Y10 -> shared eo_tonemap -> JPEG, served by
the running daemon) against the recorded DISPLAY frame (the exact JPEG shown
live). eo_y10 and eo_jpeg are teed from the same capture frame, so at a matched
timestamp they must agree to within JPEG-quantization noise. If the tone map
ever drifts from the live feed, the difference jumps and this fails.

  # record a few seconds with EO display = native res, zoom 1, then:
  verify_replay_match.py <session_id> [--url http://127.0.0.1:8093]
                         [--root /data/recordings] [--n 8] [--thresh 3.0]

Needs Pillow + numpy (bench tool). Exit 0 = match, 1 = drift, 2 = nothing to compare.
"""
import argparse, io, struct, sys, urllib.request

IDX = struct.Struct("<QQ IIII")           # seq, t_src_ns, seg, off, len, flags

def http(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return r.read()

def jpeg_wh(d):
    i = 2
    while i < len(d) - 9 and d[i] == 0xFF:
        m = d[i+1]; ln = struct.unpack(">H", d[i+2:i+4])[0]
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            return struct.unpack(">H", d[i+7:i+9])[0], struct.unpack(">H", d[i+5:i+7])[0]
        i += 2 + ln
    return None

def load_index(cdir):
    rows = []
    with open(f"{cdir}/index.bin", "rb") as f:
        while len(b := f.read(IDX.size)) == IDX.size:
            rows.append(IDX.unpack(b))
    # session t0 = segment header field (u64 at offset 16 of data.00000)
    with open(f"{cdir}/data.00000.airec", "rb") as f:
        t0 = struct.unpack("<Q", f.read(24)[16:24])[0]
    segs = {}
    def payload(row):
        seg, off, plen = row[2], row[3], row[4]
        if seg not in segs:
            segs[seg] = open(f"{cdir}/data.{seg:05d}.airec", "rb")
        segs[seg].seek(off + 64)
        return segs[seg].read(plen)
    return rows, t0, payload

def main():
    import numpy as np
    from PIL import Image
    ap = argparse.ArgumentParser()
    ap.add_argument("sid")
    ap.add_argument("--url", default="http://127.0.0.1:8093")   # or the app :8080 (+/rec)
    ap.add_argument("--root", default="/data/recordings")
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--thresh", type=float, default=3.0)
    a = ap.parse_args()
    sdir = f"{a.root}/{a.sid}"
    base = a.url + "/rec" if a.url.rstrip("/").endswith("8080") else a.url

    rows, t0, disp_payload = load_index(f"{sdir}/eo_jpeg")
    if not rows:
        print("no display frames"); sys.exit(2)

    http(f"{base}/replay/ctl?open={a.sid}")
    http(f"{base}/replay/ctl?video=native")
    checked, drifted, worst = 0, 0, 0.0
    for k in range(a.n):
        row = rows[int((len(rows) - 1) * k / max(1, a.n - 1))]
        dj = disp_payload(row)
        wh = jpeg_wh(dj)
        rel_ms = max(0, (row[1] - t0) // 1_000_000)
        nat = http(f"{base}/replay/frame?t={rel_ms}")
        nwh = jpeg_wh(nat)
        if not wh or not nwh or wh != nwh:
            continue                              # display not at native res -> skip
        da = np.asarray(Image.open(io.BytesIO(dj)).convert("L"), np.int16)
        na = np.asarray(Image.open(io.BytesIO(nat)).convert("L"), np.int16)
        diff = float(np.abs(da - na).mean())
        checked += 1; worst = max(worst, diff); drifted += diff > a.thresh
        print(f"  t={rel_ms}ms {wh[0]}x{wh[1]}  mean|delta|={diff:.2f}")
    http(f"{base}/replay/ctl?close=1")
    if not checked:
        print("no native-res display frames to compare — record with display=native, zoom 1")
        sys.exit(2)
    ok = drifted == 0
    print(f"checked {checked}  worst={worst:.2f} (thresh {a.thresh})  -> "
          + ("MATCH: native replay == live tone map" if ok else "DRIFT: replay differs from live"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
