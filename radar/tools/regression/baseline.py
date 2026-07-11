#!/usr/bin/env python3
# Freeze each fixture's OWN baseline fingerprint, then check future runs against it.
#   python baseline.py freeze      -> measure all fixtures, write baseline.json
#   python baseline.py check <bin> <fixtureID>  -> compare one run to its frozen baseline
import struct, sys, json, os
import numpy as np
FIX = {"T1":"radar human test sterile","T2":"night human 350m","T3":"highway",
       "T4":"human far day 370m","T5":"junction crossers 370m"}
BASE = os.path.join(os.path.dirname(__file__),"baseline.json")
def measure(path):
    frames=[]
    with open(path,"rb") as f:
        while True:
            h=f.read(12)
            if len(h)<12: break
            t,n=struct.unpack("<di",h)
            p=np.frombuffer(f.read(20*n),np.float32).reshape(n,5) if n else np.zeros((0,5),np.float32)
            frames.append((t,p))
    ts=np.array([t for t,_ in frames]); dt=np.diff(ts)
    comb=[];mv=[];csnr=[];ssnr=[];ppf=[]
    for _,p in frames:
        ppf.append(len(p))
        if len(p)==0: comb.append(0);mv.append(0);continue
        v=np.abs(p[:,3]);s=p[:,4];c=(v>=3)&(s<20)
        comb.append(int(c.sum()));csnr+=list(s[c]);mv.append(int(((v>=0.7)&(s>=22)).sum()));ssnr+=list(s[v<0.3])
    return dict(frames=len(frames),rate=round(float(1/np.median(dt)),2),
        ppf=int(np.median(ppf)),false_mv=int(np.median(comb)),
        false_snr=round(float(np.median(csnr)),1),static_snr=round(float(np.median(ssnr)),1),
        real_mv=round(float(np.mean(mv)),2))
if sys.argv[1]=="freeze":
    d="fixtures"; out={}
    for k in FIX:
        m=measure(os.path.join(d,k+".bin")); out[k]=m
        print("  %-3s %-26s rate %.1f  pts %3d  false %3d @%.0fdB  static %.0fdB  realmv %.2f"%(
            k,FIX[k],m["rate"],m["ppf"],m["false_mv"],m["false_snr"],m["static_snr"],m["real_mv"]))
    json.dump(out,open(BASE,"w"),indent=1); print("froze",BASE)
else:
    m=measure(sys.argv[2]); k=sys.argv[3]; b=json.load(open(BASE))[k]
    def g(name,now,ref,ok): print(("  PASS" if ok else "  FAIL")+"  %-16s now %s  base %s"%(name,now,ref))
    print("=== check %s vs baseline %s ==="%(k,FIX[k]))
    g("rate",m["rate"],b["rate"],m["rate"]>=b["rate"]-0.8)
    g("points/frame",m["ppf"],b["ppf"],abs(m["ppf"]-b["ppf"])<=0.15*b["ppf"])
    g("false-movers",m["false_mv"],b["false_mv"],m["false_mv"]<=b["false_mv"]*1.15+3)
    g("false-mv SNR",m["false_snr"],b["false_snr"],m["false_snr"]<=b["false_snr"]+2)
    g("static SNR",m["static_snr"],b["static_snr"],m["static_snr"]>=b["static_snr"]-2)
    g("real-movers",m["real_mv"],b["real_mv"],m["real_mv"]>=b["real_mv"]*0.9)
