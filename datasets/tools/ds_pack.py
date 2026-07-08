#!/usr/bin/env python3
"""Assemble the final COCO files, folding in human review decisions.

Reads merged_labels.json + split_manifest.json + review/corrections.json
(if the review app ran) and writes annotations/instances_{train,val,test}.json
per the frozen schema. Review decisions: accept -> reviewed_ok, edit ->
reviewed_edited (new box/label, label_source human, score 1.0), reject ->
never packed (stays in the corrections ledger). Un-reviewed queued boxes fail
the pack unless --pending=drop (then dropped + ledgered LABELS_LOW_CONF_ALL).

Re-asserts every invariant before writing (SPEC.md §7); any failure aborts.

  ds_pack.py --root <DATASET_ROOT> [--pending fail|drop]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, read_json, write_json
from airpoc_ds import ledger
from airpoc_ds.coco import annotation_record, instances_skeleton
from airpoc_ds.schema import validate


def apply_corrections(annotations, corrections):
    """-> (packed_annotations, n_reviewed, n_rejected, pending)"""
    decisions = corrections.get("annotation_decisions", {})
    out, pending = [], []
    n_reviewed = n_rejected = 0
    for ann in annotations:
        if ann["review_status"] == "auto_ok":
            out.append(ann)
            continue
        dec = decisions.get(str(ann["id"]))
        if dec is None:
            pending.append(ann)
            continue
        n_reviewed += 1
        if dec["action"] == "reject":
            n_rejected += 1
            continue
        if dec["action"] == "accept":
            fixed = dict(ann, review_status="reviewed_ok", score=1.0,
                         label_source="human")
            if fixed["category_id"] is None:  # REVIEW-routed needs a class
                pending.append(ann)
                n_reviewed -= 1
                continue
            out.append(fixed)
        elif dec["action"] == "edit":
            bbox = dec.get("bbox", ann["bbox"])
            cat = dec.get("category_id", ann["category_id"])
            if cat not in (1, 2):
                pending.append(ann)
                n_reviewed -= 1
                continue
            fixed = annotation_record(ann["image_id"], cat, bbox, 1.0, "human",
                                      "reviewed_edited", ann["raw_label"],
                                      ann["cross_check"])
            out.append(fixed)
    return out, n_reviewed, n_rejected, pending


def check(cond, msg, errors):
    if not cond:
        errors.append(msg)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--pending", choices=("fail", "drop"), default="fail",
                    help="what to do with queued-but-unreviewed boxes")
    args = ap.parse_args()

    root = dataset_root(args.root)
    merged = read_json(root / "annotations" / "merged_labels.json")
    split_manifest = read_json(root / "splits" / "split_manifest.json")
    corr_path = root / "review" / "corrections.json"
    corrections = read_json(corr_path) if corr_path.exists() else {}

    packed, n_reviewed, n_rejected, pending = apply_corrections(
        merged["annotations"], corrections)

    # human-drawn boxes (e.g. the missed tiny target on an empty terminal frame)
    for add in corrections.get("added_annotations", []):
        packed.append(annotation_record(add["image_id"], add["category_id"],
                                        add["bbox"], 1.0, "human",
                                        "reviewed_edited", "human_added"))
        n_reviewed += 1
    if pending:
        if args.pending == "fail":
            sys.exit(f"{len(pending)} queued boxes still unreviewed — run the "
                     "review app (review_app/ds_review_server.py) or pass "
                     "--pending=drop to drop them ledgered")
        img_by_id = {i["id"]: i for i in merged["images"]}
        ledger.exclude_frames(root, [{
            "source_video_id": img_by_id[a["image_id"]]["source_video_id"],
            "timestamp_ms": img_by_id[a["image_id"]]["timestamp_ms"],
            "extraction_mode": img_by_id[a["image_id"]]["extraction_mode"],
            "reason_code": "LABELS_LOW_CONF_ALL",
            "detail": f"unreviewed box {a['id']} dropped at pack",
        } for a in pending if a["image_id"] in img_by_id])

    assignments = split_manifest["assignments"]
    ann_by_image = {}
    for a in packed:
        ann_by_image.setdefault(a["image_id"], []).append(a)

    errors = []
    files = {s: instances_skeleton(s) for s in ("train", "val", "test")}
    dropped_empty = []
    for img in merged["images"]:
        anns = ann_by_image.get(img["id"], [])
        if not anns:
            dropped_empty.append(img)
            continue
        vid = img["source_video_id"]
        check(vid in assignments, f"video {vid} missing from split manifest", errors)
        split = assignments.get(vid, "train")
        img = dict(img, split=split)
        files[split]["images"].append(img)
        files[split]["annotations"].extend(anns)
        for a in anns:
            check(a["category_id"] in (1, 2),
                  f"annotation {a['id']} bad category {a['category_id']}", errors)
            x, y, w, h = a["bbox"]
            check(x >= 0 and y >= 0 and x + w <= img["width"] + 0.5
                  and y + h <= img["height"] + 0.5,
                  f"annotation {a['id']} bbox out of bounds", errors)

    # cross-file invariants
    seen_images = set()
    for split, coco in files.items():
        ids = [i["id"] for i in coco["images"]]
        check(len(ids) == len(set(ids)), f"{split}: duplicate image ids", errors)
        check(not (set(ids) & seen_images), f"{split}: image leaks across splits", errors)
        seen_images.update(ids)
        img_set = set(ids)
        ann_ids = [a["id"] for a in coco["annotations"]]
        check(len(ann_ids) == len(set(ann_ids)), f"{split}: duplicate annotation ids", errors)
        for a in coco["annotations"]:
            check(a["image_id"] in img_set,
                  f"{split}: annotation {a['id']} references missing image", errors)

    queue = read_json(root / "review" / "review_queue.json")
    check(queue["queued"] <= queue["budget"], "review budget exceeded", errors)

    if errors:
        for e in errors[:20]:
            print(f"INVARIANT FAILED: {e}", file=sys.stderr)
        sys.exit(f"pack aborted: {len(errors)} invariant failures")

    if dropped_empty:
        ledger.exclude_frames(root, [{
            "source_video_id": i["source_video_id"],
            "timestamp_ms": i["timestamp_ms"],
            "extraction_mode": i["extraction_mode"],
            "reason_code": "EMPTY_GARBAGE",
            "detail": "no annotation survived review",
        } for i in dropped_empty])

    for split, coco in files.items():
        validate(coco, "coco_instances")
        write_json(root / "annotations" / f"instances_{split}.json", coco)
        print(f"instances_{split}.json: {len(coco['images'])} images, "
              f"{len(coco['annotations'])} annotations")
    print(f"review: {n_reviewed} human-reviewed ({n_rejected} rejected), "
          f"{len(pending)} pending-{args.pending}, "
          f"{len(dropped_empty)} images dropped empty")


if __name__ == "__main__":
    main()
