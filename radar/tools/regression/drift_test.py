#!/usr/bin/env python3
# Does the reported angle of FIXED objects drift with uptime? (calibration-drift signature)
# For each strong persistent static reflector (fixed range bin), track its az/el/snr
# and the comb rate over the session. Fixed object + drifting angle = drift evidence.
import struct, sys
import numpy as np
path = sys.argv[1]
frames=[]
with open(path,"rb") as f:
    while True:
        h=f.read(12)
        if len(h)<12: break
        t,n=struct.unpack("<di",h)
        p=np.frombuffer(f.read(20*n),np.float32).reshape(n,5) if n else np.zeros((0,5),np.float32)
        frames.append((t,p))
T=frames[-1][0]-frames[0][0]
print("%s  %d frames  %.0fs"%(path,len(frames),T))
# find persistent strong static reflectors: (2m range, 2deg az) cells hit often with snr>=30
from collections import Counter
cnt=Counter()
for t,p in frames[::5]:
    st=p[(np.abs(p[:,3])<0.3)&(p[:,4]>=30)]
    for r,az,el,v,s in st: cnt[(int(r/2),int(az//2))]+=1
top=[k for k,c in cnt.most_common(6) if c> len(frames[::5])*0.5]
print("reflectors (range~, az~):",[(k[0]*2,k[1]*2) for k in top])
# track each reflector's az/el per 30s chunk
NCH=max(int(T//30),4)
for k in top[:4]:
    r0,a0=k[0]*2,k[1]*2
    rows=[]
    for ci in range(NCH):
        t0,t1=ci*T/NCH,(ci+1)*T/NCH
        az=[];el=[];sn=[]
        for t,p in frames:
            tt=t-frames[0][0]
            if not(t0<=tt<t1): continue
            m=(np.abs(p[:,0]-(r0+1))<2.5)&(np.abs(p[:,1]-(a0+1))<2.5)&(np.abs(p[:,3])<0.3)&(p[:,4]>=25)
            w=p[m]
            if len(w):
                j=int(np.argmax(w[:,4])); az.append(w[j,1]); el.append(w[j,2]); sn.append(w[j,4])
        if az: rows.append((0.5*(t0+t1),np.median(az),np.median(el),np.median(sn),len(az)))
    if len(rows)>=3:
        rows=np.array(rows)
        print(" reflector r=%dm az=%d: az drift %+.2f deg (%.2f->%.2f)  el drift %+.2f (%.2f->%.2f)  snr %.0f->%.0f"%(
            r0,a0,rows[-1,1]-rows[0,1],rows[0,1],rows[-1,1],
            rows[-1,2]-rows[0,2],rows[0,2],rows[-1,2],rows[0,3],rows[-1,3]))
        # jitter within chunks
        print("   per-chunk az: "+" ".join("%.2f"%x for x in rows[:,1]))
        print("   per-chunk el: "+" ".join("%.2f"%x for x in rows[:,2]))
# comb rate over time (uptime-dependent noise?)
print(" comb-rate per chunk (false movers/frame):")
out=[]
for ci in range(NCH):
    t0,t1=ci*T/NCH,(ci+1)*T/NCH
    c=[];
    for t,p in frames:
        tt=t-frames[0][0]
        if not(t0<=tt<t1) or len(p)==0: continue
        c.append(int(((np.abs(p[:,3])>=3)&(p[:,4]<20)).sum()))
    if c: out.append("%.0f"%np.median(c))
print("   "+" ".join(out))
