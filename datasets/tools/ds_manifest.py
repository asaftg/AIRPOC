#!/usr/bin/env python3
"""Write manifest.json — the dataset card + content seal (SPEC.md §8).

Freezes what this build IS: pinned catalog commit, counts (with the honest
used/indexed arithmetic), review outcomes, sha256 of every packed artifact,
and the use restriction. Run last; ds_validate.py re-checks everything
against it, so any post-hoc drift is caught.

  ds_manifest.py --root <DATASET_ROOT> --version 0.1.0
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import SPEC_VERSION, USE_RESTRICTION, dataset_root, read_json, write_json
from airpoc_ds import ledger
from airpoc_ds.hashing import sha256_file
from airpoc_ds.schema import validate


def tool_versions():
    v = {"python": sys.version.split()[0]}
    try:
        out = subprocess.run(["ffmpeg", "-version"], capture_output=True,
                             text=True, check=True).stdout.splitlines()[0]
        v["ffmpeg"] = out.split()[2]
    except Exception:  # noqa: BLE001
        v["ffmpeg"] = "n/a"
    for mod, key in (("torch", "torch"), ("transformers", "grounding_dino"),
                     ("mmdet", "rtmdet")):
        try:
            v[key] = __import__(mod).__version__
        except ImportError:
            v[key] = "n/a"
    return v


def pipeline_commit():
    try:
        return subprocess.run(["git", "-C", str(Path(__file__).parent),
                               "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except Exception:  # noqa: BLE001
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--version", required=True, help="dataset version, e.g. 0.1.0")
    ap.add_argument("--name", default="airpoc_fpv_vh")
    args = ap.parse_args()
    root = dataset_root(args.root)

    snapshot = read_json(root / "catalog" / "catalog_snapshot.json")
    index_path = root / "videos" / "index.json"
    index = read_json(index_path) if index_path.exists() else {}
    queue = read_json(root / "review" / "review_queue.json")
    excl = ledger.load(root)

    images, annotations = [], []
    splits = {}
    for split in ("train", "val", "test"):
        coco = read_json(root / "annotations" / f"instances_{split}.json")
        images += coco["images"]
        annotations += coco["annotations"]
        splits[split] = len(coco["images"])

    used_videos = {i["source_video_id"] for i in images}
    params = None
    for vdir in sorted((root / "frames").iterdir()):
        p = vdir / "images.json"
        if p.exists():
            params = read_json(p)["extraction_params"]
            break

    by_class = {"vehicle": 0, "human": 0}
    small = {"vehicle": 0, "human": 0}
    reviewed = 0
    for a in annotations:
        name = "vehicle" if a["category_id"] == 1 else "human"
        by_class[name] += 1
        if 10.0 <= a["bbox_max_side_px"] <= 40.0:
            small[name] += 1
        if a["label_source"] == "human":
            reviewed += 1

    dropped_frames = len(excl["frames"])
    manifest = {
        "dataset_name": args.name,
        "dataset_version": args.version,
        "spec_version": SPEC_VERSION,
        "built_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "pipeline_commit": pipeline_commit(),
        "tool_versions": tool_versions(),
        "source": {
            "catalog_repo": snapshot["catalog_repo"],
            "catalog_commit": snapshot["catalog_commit"],
            "catalog_snapshot_sha256": sha256_file(root / "catalog" / "catalog_snapshot.json"),
            "videos_indexed": len(snapshot["videos"]),
            "videos_mirrored": len(index),
            "videos_used": len(used_videos),
        },
        "classes": {"1": "vehicle", "2": "human"},
        "extraction_params": params or {"baseline_fps": 0, "terminal_tail_s": 0,
                                        "terminal_fps": 0},
        "counts": {
            "images": len(images),
            "annotations": len(annotations),
            "by_class": by_class,
            "by_mode": {m: sum(1 for i in images if i["extraction_mode"] == m)
                        for m in ("baseline", "terminal")},
            "from_replay": sum(1 for i in images if i["from_replay"]),
            "small_10_40px": small,
            "review": {"auto_accepted": sum(1 for a in annotations
                                            if a["review_status"] == "auto_ok"),
                       "human_reviewed": reviewed,
                       "dropped": dropped_frames + queue["overflow_dropped"]},
        },
        "splits": splits,
        "hashes": {
            "instances_train_sha256": sha256_file(root / "annotations" / "instances_train.json"),
            "instances_val_sha256": sha256_file(root / "annotations" / "instances_val.json"),
            "instances_test_sha256": sha256_file(root / "annotations" / "instances_test.json"),
            "split_manifest_sha256": sha256_file(root / "splits" / "split_manifest.json"),
            "stats_sha256": sha256_file(root / "reports" / "stats.json"),
        },
        "exclusions_count": len({v["stem"] for v in excl["videos"]}),
        "use_restriction": USE_RESTRICTION,
    }
    validate(manifest, "manifest")
    write_json(root / "manifest.json", manifest)
    print(f"manifest.json sealed: {args.name} {args.version} — "
          f"{len(images)} images, {len(annotations)} boxes, "
          f"{len(used_videos)}/{len(snapshot['videos'])} videos used")


if __name__ == "__main__":
    main()
