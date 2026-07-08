#!/usr/bin/env python3
"""ffprobe every mirrored MP4 -> <video_id>.probe.json.

Records what extraction needs (duration, fps, geometry, pix_fmt, color_range,
codec) plus the sha256 and source stem/url from videos/index.json. Videos
ffprobe cannot read are ledgered PROBE_FAIL.

  ds_probe.py --root <DATASET_ROOT> [--force]
"""

import argparse
import json
import subprocess
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, read_json, write_json
from airpoc_ds import ledger


def ffprobe(path: Path) -> dict:
    cmd = ["ffprobe", "-v", "error", "-select_streams", "v:0",
           "-show_entries",
           "stream=width,height,pix_fmt,color_range,avg_frame_rate,codec_name",
           "-show_entries", "format=duration",
           "-of", "json", str(path)]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    data = json.loads(out)
    stream = data["streams"][0]
    fps = float(Fraction(stream["avg_frame_rate"])) if stream.get("avg_frame_rate") not in (None, "0/0") else 0.0
    return {
        "duration_s": float(data["format"]["duration"]),
        "fps": round(fps, 4),
        "width": int(stream["width"]),
        "height": int(stream["height"]),
        "pix_fmt": stream.get("pix_fmt", "unknown"),
        "color_range": stream.get("color_range", "unknown"),
        "codec": stream.get("codec_name", "unknown"),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--force", action="store_true", help="re-probe existing")
    args = ap.parse_args()

    root = dataset_root(args.root)
    index = read_json(root / "videos" / "index.json")
    ok = fail = skipped = 0
    for stem, entry in sorted(index.items()):
        vid = entry["video_id"]
        mp4 = root / "videos" / f"{vid}.mp4"
        out = root / "videos" / f"{vid}.probe.json"
        if out.exists() and not args.force:
            skipped += 1
            continue
        try:
            probe = ffprobe(mp4)
        except (subprocess.CalledProcessError, KeyError, ValueError, IndexError) as e:
            detail = getattr(e, "stderr", "") or str(e)
            ledger.exclude_video(root, stem, "PROBE_FAIL", detail=str(detail)[:500],
                                 video_id=vid)
            print(f"PROBE_FAIL {vid}: {detail}")
            fail += 1
            continue
        probe.update(sha256=entry["sha256"], stem=stem, url=entry["url"])
        write_json(out, probe)
        ok += 1
    print(f"probed {ok}, skipped {skipped} existing, {fail} failed")
    if fail:
        sys.exit(1)


if __name__ == "__main__":
    main()
