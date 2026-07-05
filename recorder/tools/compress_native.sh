#!/bin/bash
# compress_native — WORKSTATION archival: eo_y10 (y10p) -> lossless FFV1 mkv.
#   ./compress_native.sh <session_dir>
# Unpacks the 10-bit bitstream to gray16le and pipes into ffmpeg FFV1 (~2-3x).
# Verify round-trip losslessness before deleting originals.
set -euo pipefail
DIR=${1:?usage: compress_native.sh <session_dir>}
OUT="$DIR/eo_y10_ffv1.mkv"
W=1440 H=1088 FPS=60

python3 - "$DIR" <<'PY' |
import struct, sys, os
IDX = struct.Struct("<QQ IIII")
d = os.path.join(sys.argv[1], "eo_y10")
rows = []
with open(os.path.join(d, "index.bin"), "rb") as f:
    while True:
        b = f.read(IDX.size)
        if len(b) < IDX.size: break
        rows.append(IDX.unpack(b))
segs = {}
out = sys.stdout.buffer
for seq, ts, seg, off, plen, flags in rows:
    if seg not in segs:
        segs[seg] = open(os.path.join(d, f"data.{seg:05d}.airec"), "rb")
    f = segs[seg]
    f.seek(off + 64)
    packed = f.read(plen)
    # y10p -> gray16le (values left in the low 10 bits)
    n = len(packed) // 5 * 4
    o = bytearray(n * 2)
    mv = memoryview(packed)
    for g in range(len(packed) // 5):
        v = int.from_bytes(mv[g*5:g*5+5], "little")
        for k in range(4):
            px = (v >> (10 * k)) & 0x3FF
            struct.pack_into("<H", o, (g*4 + k) * 2, px)
    out.write(o)
PY
ffmpeg -hide_banner -f rawvideo -pixel_format gray16le -video_size ${W}x${H} \
       -framerate $FPS -i - -c:v ffv1 -level 3 -g 1 -slices 8 -y "$OUT"
echo "wrote $OUT"
