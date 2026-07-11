#!/usr/bin/env python3
# Phase-0 safety net. Run any radar recording through the 6 "don't get worse" gates.
# Usage: python regression_gates.py <points.bin> <rj|rh>
import struct, sys
import numpy as np
path, kind = sys.argv[1], sys.argv[2]
frames=[]
with open(path,"rb") as f:
    while True:
        h=f.read(12)
        if len(h)<12: break
        t,n=struct.unpack("<di",h)
        p=np.frombuffer(f.read(20*n),np.float32).reshape(n,5) if n else np.zeros((0,5),np.float32)
        frames.append((t,p))
ts=np.array([t for t,_ in frames]); dt=np.diff(ts)
rate=1.0/np.median(dt); drops=int((dt>0.09).sum())
ppf=np.array([len(p) for _,p in frames]); ppf_med=np.median(ppf)
comb=[]; strongmv=[]; comb_snr=[]; stat_snr=[]
for _,p in frames:
    if len(p)==0: comb.append(0); strongmv.append(0); continue
    v=np.abs(p[:,3]); s=p[:,4]
    c=(v>=3)&(s<20); comb.append(int(c.sum())); comb_snr+=list(s[c])
    strongmv.append(int(((v>=0.7)&(s>=22)).sum()))
    stat_snr+=list(s[v<0.3])
comb_med=np.median(comb); strongmv_med=np.mean(strongmv)
comb_snr_med=np.median(comb_snr) if comb_snr else 0
stat_snr_med=np.median(stat_snr) if stat_snr else 0
mv_floor=0.40 if kind=="rj" else 0.50
def line(name,val,ok):
    print(("  PASS" if ok else "  FAIL")+"  %-34s %s"%(name,val))
print("\n=== %s (%s) — %d frames ==="%(path.split('/')[-1],kind,len(frames)))
line("rate >= 25.5 Hz, 0 drops", "%.1f Hz, %d drops"%(rate,drops), rate>=25.5 and drops==0)
line("points/frame in 280-340", "%d"%ppf_med, 280<=ppf_med<=340)
line("false-movers/frame <= 105", "%d"%comb_med, comb_med<=105)
line("false-mover SNR <= 19", "%.1f dB"%comb_snr_med, comb_snr_med<=19)
line("static SNR >= 24", "%.1f dB"%stat_snr_med, stat_snr_med>=24)
line("real movers/frame >= %.2f"%mv_floor, "%.2f"%strongmv_med, strongmv_med>=mv_floor)
print("  (tangential-detected/frame today = 0 ; any future >0 = improvement, never a fail)")
