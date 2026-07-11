#!/usr/bin/env python3
# On-box: AIREC radar_wire -> compact points.bin  [double t, int32 n, n*(5 f32: r,az,el,v,snr)]
import glob, struct, json, os, sys
SESS, OUT = sys.argv[1], sys.argv[2]
def read_chan(chan):
    out=[]
    for f in sorted(glob.glob(os.path.join(SESS,chan,"data.*.airec"))):
        b=open(f,"rb").read(); off=64
        while off+64<=len(b):
            if b[off:off+4]!=b"REC0": break
            ts=struct.unpack_from("<Q",b,off+16)[0]
            plen,fl=struct.unpack_from("<II",b,off+32); pay=b[off+64:off+64+plen]; off+=(64+plen+7)&~7
            if fl&4: continue
            out.append((ts,pay))
    return out
rd=read_chan("radar_wire")
if not rd: print("NO radar_wire"); sys.exit(1)
t0=rd[0][0]; nf=0
with open(OUT,"wb") as o:
    for ts,pay in rd:
        try: fr=json.loads(pay.decode("utf-8","replace"))
        except: continue
        pts=fr.get("points",[])
        o.write(struct.pack("<di",(ts-t0)/1e9,len(pts)))
        for p in pts:
            o.write(struct.pack("<5f",p.get("r",0),p.get("az",0),p.get("el",0),p.get("v",0),p.get("snr",0)))
        nf+=1
print("wrote",OUT,nf,"frames")
