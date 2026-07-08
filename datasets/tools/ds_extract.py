#!/usr/bin/env python3
"""Decode each mirrored video once and write the sampled mono frames.

Per video: build the sampling plan (exact terminal windows from the catalog's
human-marked flight segments; interim looming-gated fallback when a video has
no annotations), then stream-decode with one ffmpeg process (rawvideo RGB on
a pipe — nothing intermediate on disk), convert to 8-bit luma with the frozen
coefficients, blank the catalog's exclusion_masks at the pixel level, reject
near-duplicates (dHash, permissive in terminal mode), and save PNGs +
frames/<video_id>/images.json.

Frame timestamps assume constant frame rate (t = n/avg_fps) — correct for
these web MP4s; a VFR source would drift and should be re-muxed first.

  ds_extract.py --root <DATASET_ROOT> [--only VIDEO_ID] [--force]
"""

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import (BASELINE_FPS, DEDUP_HAMMING_BASELINE, DEDUP_HAMMING_TERMINAL,
                       DEDUP_WINDOW_S, TERMINAL_FPS, TERMINAL_TAIL_S,
                       dataset_root, read_json, write_json)
from airpoc_ds import ledger
from airpoc_ds.coco import image_record
from airpoc_ds.dedup import TemporalDeduper
from airpoc_ds.hashing import sha256_bytes
from airpoc_ds.luma import rgb_to_luma8
from airpoc_ds.terminal import sample_plan


def blank_masks(gray: np.ndarray, masks):
    h, w = gray.shape
    for m in masks or []:
        x0, y0 = max(0, int(m["x"])), max(0, int(m["y"]))
        x1 = min(w, int(m["x"] + m["w"]))
        y1 = min(h, int(m["y"] + m["h"]))
        if x1 > x0 and y1 > y0:
            gray[y0:y1, x0:x1] = 0
    return gray


def extract_video(root, vid, rec, probe, args):
    """-> (n_saved, n_dedup_dropped) or None on decode failure."""
    mp4 = root / "videos" / f"{vid}.mp4"
    out_dir = root / "frames" / vid
    out_dir.mkdir(parents=True, exist_ok=True)

    segments = rec["segments"]
    terminal_source = "segments" if segments else "looming_heuristic"
    plan = sample_plan(segments, probe["duration_s"],
                       args.baseline_fps, args.terminal_fps, args.tail_s)
    if not plan:
        return 0, 0

    w, h = probe["width"], probe["height"]
    frame_bytes = w * h * 3
    cmd = ["ffmpeg", "-nostdin", "-v", "error", "-i", str(mp4),
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            bufsize=frame_bytes * 4)

    dedup = {"b": TemporalDeduper(DEDUP_WINDOW_S, DEDUP_HAMMING_BASELINE),
             "t": TemporalDeduper(DEDUP_WINDOW_S, DEDUP_HAMMING_TERMINAL)}
    images, dropped = [], 0
    pi = 0          # next plan entry to satisfy
    n = 0           # decoded frame counter
    frame_index = 0
    fps = probe["fps"] or 30.0
    try:
        while pi < len(plan):
            buf = proc.stdout.read(frame_bytes)
            if len(buf) < frame_bytes:
                break  # end of stream (plan tail past the last frame is fine)
            t_ms = int(round(n * 1000.0 / fps))
            n += 1
            if t_ms < plan[pi]["t_ms"]:
                continue
            entry = plan[pi]
            pi += 1
            # collapse plan entries that landed on the same decoded frame
            while pi < len(plan) and plan[pi]["t_ms"] <= t_ms:
                if plan[pi]["mode"] == "t":
                    entry = plan[pi]
                pi += 1

            rgb = np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3)
            gray, conversion = rgb_to_luma8(rgb)  # ffmpeg rgb24 is full-range
            gray = blank_masks(gray.copy(), rec.get("exclusion_masks"))
            if dedup[entry["mode"]].is_duplicate(t_ms, gray):
                dropped += 1
                continue

            name = f"{vid}_{t_ms:08d}_{entry['mode']}.png"
            png_path = out_dir / name
            Image.fromarray(gray).save(png_path, optimize=True)
            img = image_record(vid, t_ms, entry["mode"], w, h, probe, conversion,
                               sha256_bytes(png_path.read_bytes()),
                               from_replay=entry["from_replay"],
                               terminal_source=terminal_source,
                               exclusion_masks=rec.get("exclusion_masks") or None)
            img["frame_index"] = frame_index
            frame_index += 1
            images.append(img)
    finally:
        proc.stdout.close()
        stderr = proc.stderr.read().decode(errors="replace")
        proc.wait()

    if not images and proc.returncode != 0:
        ledger.exclude_video(root, rec["stem"], "UNREADABLE_DECODE",
                             detail=stderr[:500], video_id=vid)
        return None

    write_json(out_dir / "images.json", {
        "video_id": vid,
        "terminal_source": terminal_source,
        "extraction_params": {"baseline_fps": args.baseline_fps,
                              "terminal_fps": args.terminal_fps,
                              "terminal_tail_s": args.tail_s},
        "dedup_dropped": dropped,
        "images": images,
    })
    return len(images), dropped


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--only", help="extract a single video_id")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--baseline-fps", type=float, default=BASELINE_FPS)
    ap.add_argument("--terminal-fps", type=float, default=TERMINAL_FPS)
    ap.add_argument("--tail-s", type=float, default=TERMINAL_TAIL_S)
    args = ap.parse_args()

    root = dataset_root(args.root)
    snapshot = read_json(root / "catalog" / "catalog_snapshot.json")
    by_stem = {r["stem"]: r for r in snapshot["videos"]}
    index = read_json(root / "videos" / "index.json")

    total = skipped = failed = 0
    for stem, entry in sorted(index.items()):
        vid = entry["video_id"]
        if args.only and vid != args.only:
            continue
        if (root / "frames" / vid / "images.json").exists() and not args.force:
            skipped += 1
            continue
        probe_path = root / "videos" / f"{vid}.probe.json"
        if not probe_path.exists():
            print(f"skip {vid}: no probe (run ds_probe.py)")
            continue
        result = extract_video(root, vid, by_stem[stem], read_json(probe_path), args)
        if result is None:
            print(f"UNREADABLE_DECODE {vid}")
            failed += 1
            continue
        saved, dropped = result
        total += saved
        print(f"{vid}: {saved} frames ({dropped} dedup-dropped)")

    print(f"extracted {total} frames; {skipped} videos already done, {failed} unreadable")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
