#!/usr/bin/env python3
"""Bench diagnostic: dump raw TLV types / point counts / SNR from the
AWR2944P data UART. Standalone (stdlib + pyserial) — use it to cross-check
what the firmware actually emits before/while running the C daemon, e.g.
to confirm whether TLV 7 (SideInfo/SNR) is present on this build.

    python3 radar_tlv_probe.py --port /dev/radar-data --baud 3125000

This is a TOOL (bench only), not part of the shipping datapath — the
production parser is radar/src/tlv.c. Keep it out of the hot path.
"""
import argparse
import struct
import sys

MAGIC = b"\x02\x01\x04\x03\x06\x05\x08\x07"
HDR = struct.Struct("<IIIIIIII")   # version,totalLen,platform,frame,cycles,nObj,nTLV,sub


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/radar-data")
    ap.add_argument("--baud", type=int, default=3125000)
    ap.add_argument("--frames", type=int, default=50, help="stop after N frames (0=forever)")
    args = ap.parse_args()

    try:
        import serial  # pyserial
    except ImportError:
        print("pip install pyserial", file=sys.stderr)
        return 1

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    buf = bytearray()
    seen = 0
    last_frame = None
    while args.frames == 0 or seen < args.frames:
        buf.extend(ser.read(4096))
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                if len(buf) > 7:
                    del buf[: len(buf) - 7]
                break
            if i:
                del buf[:i]
            if len(buf) < 8 + HDR.size:
                break
            _, total, _, frame, _, nobj, ntlv, _ = HDR.unpack_from(buf, 8)
            if total < 8 + HDR.size or total > 256 * 1024:
                del buf[:1]
                continue
            if len(buf) < total:
                break
            pkt = bytes(buf[:total])
            del buf[:total]

            types, snr_lo, snr_hi = [], None, None
            cur = 8 + HDR.size
            while cur + 8 <= total:
                t, ln = struct.unpack_from("<II", pkt, cur)
                cur += 8
                types.append(t)
                if t == 7:  # SideInfo: n x (snr,noise) int16 x0.1dB
                    n = ln // 4
                    vals = [struct.unpack_from("<hh", pkt, cur + k * 4)[0] * 0.1 for k in range(n)]
                    if vals:
                        snr_lo, snr_hi = min(vals), max(vals)
                cur += ln

            gap = "" if last_frame is None or frame == last_frame + 1 else f"  <<GAP {frame-last_frame-1}"
            last_frame = frame
            snr = f"  snr[{snr_lo:.1f}..{snr_hi:.1f}]dB" if snr_lo is not None else "  (no SideInfo)"
            print(f"frame {frame:6d}  pts {nobj:3d}  TLVs {types}{snr}{gap}")
            seen += 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
