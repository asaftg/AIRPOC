#!/usr/bin/env python3
"""Deterministic class-aware split BY VIDEO (SPEC.md §6).

Frames of one video are near-duplicates — a frame-level split leaks val/test
into train. Method hash_stratified_v1: sha1(seed:video_id) bucketing into
80/10/10, stratified so videos containing human boxes (the rarer class)
spread across all three splits; a deterministic fix-up guarantees no split
starves when a stratum has >= 3 videos. A video's assignment is stable when
new videos are added (pure hash), except within the fix-up of a small stratum.

  ds_split.py --root <DATASET_ROOT> [--seed N]
"""

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import SPEC_VERSION, dataset_root, read_json, write_json
from airpoc_ds.schema import validate

RATIOS = {"train": 0.8, "val": 0.1, "test": 0.1}
DEFAULT_SEED = 20260708


def bucket(seed: int, video_id: str) -> float:
    h = hashlib.sha1(f"{seed}:{video_id}".encode()).hexdigest()
    return int(h, 16) / float(1 << 160)


def assign_stratum(video_ids, seed):
    """Hash-assign one stratum, then fix up empty val/test if it has >=3 videos."""
    u = {v: bucket(seed, v) for v in video_ids}
    out = {}
    for v in video_ids:
        if u[v] < RATIOS["train"]:
            out[v] = "train"
        elif u[v] < RATIOS["train"] + RATIOS["val"]:
            out[v] = "val"
        else:
            out[v] = "test"
    if len(video_ids) >= 3:
        by_u = sorted(video_ids, key=lambda v: (u[v], v))
        for split in ("val", "test"):
            if split not in out.values():
                donor = next(v for v in reversed(by_u) if out[v] == "train")
                out[donor] = split
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    args = ap.parse_args()

    root = dataset_root(args.root)
    merged = read_json(root / "annotations" / "merged_labels.json")

    videos = sorted({img["source_video_id"] for img in merged["images"]})
    human_imgs = {a["image_id"] for a in merged["annotations"]
                  if a["category_id"] == 2}
    img_video = {img["id"]: img["source_video_id"] for img in merged["images"]}
    human_videos = sorted({img_video[i] for i in human_imgs if i in img_video})
    other_videos = [v for v in videos if v not in set(human_videos)]

    assignments = {}
    assignments.update(assign_stratum(human_videos, args.seed))
    assignments.update(assign_stratum(other_videos, args.seed))

    manifest = {
        "spec_version": SPEC_VERSION,
        "seed": args.seed,
        "ratios": RATIOS,
        "method": "hash_stratified_v1",
        "assignments": assignments,
        "invariant": "split by source_video_id; no video appears in more than one split",
    }
    validate(manifest, "split_manifest")
    write_json(root / "splits" / "split_manifest.json", manifest)

    counts = {s: sum(1 for v in assignments.values() if v == s)
              for s in ("train", "val", "test")}
    hcounts = {s: sum(1 for v in human_videos if assignments[v] == s)
               for s in ("train", "val", "test")}
    print(f"{len(videos)} videos -> {counts} (videos with humans: {hcounts})")


if __name__ == "__main__":
    main()
