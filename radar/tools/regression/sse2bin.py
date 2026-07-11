#!/usr/bin/env python3
# SSE capture (data: {...} lines) -> points.bin [double t, int32 n, n*(5f: r,az,el,v,snr)]
import json, struct, sys
src, out = sys.argv[1], sys.argv[2]
nf = 0
with open(src) as f, open(out, "wb") as o:
    for line in f:
        line = line.strip()
        if not line.startswith("data: "): continue
        try: d = json.loads(line[6:])
        except: continue
        pts = d.get("points", [])
        o.write(struct.pack("<di", float(d.get("timestamp", 0)), len(pts)))
        for p in pts:
            o.write(struct.pack("<5f", p.get("r",0), p.get("az",0), p.get("el",0),
                                p.get("v",0), p.get("snr",0)))
        nf += 1
print("frames:", nf)
