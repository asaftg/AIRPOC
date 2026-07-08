#!/usr/bin/env python3
"""Merge the two labelers into routed annotations + the capped review queue.

Per GroundingDINO box (SPEC.md §5): map the raw label through the frozen
synonym table (vehicle / human / REVIEW / absent -> IGNORE), auto-accept when
score >= 0.45 AND an RTMDet box of the same class agrees (IoU >= 0.5),
otherwise queue for human review. The queue is CAPPED (default 200 boxes,
toughest first); overflow is dropped and ledgered — never silently, never
handed to the human anyway.

Terminal frames with zero detections get a rescan request for ds_label.py
--rescan (a target SHOULD be there); frames that stay empty after the rescan
are garbage-dropped with a capped few queued as terminal_no_labels.
Baseline frames with zero kept boxes are dropped (EMPTY_GARBAGE) — that is
where sky/blur/ground-rush lives.

Writes annotations/merged_labels.json + review/review_queue.json.
Reports auto-accepted / queued / dropped honestly.

  ds_merge.py --root <DATASET_ROOT> [--budget N]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import (AUTO_ACCEPT_SCORE, CROSS_CHECK_IOU, REVIEW_BUDGET_DEFAULT,
                       SPEC_VERSION, dataset_root, read_json, write_json)
from airpoc_ds import ledger
from airpoc_ds.coco import annotation_record, clip_bbox, iou
from airpoc_ds.review import cap_queue, toughness
from airpoc_ds.schema import synonyms, validate

CROWDED_BOXES = 8  # more accepted+queued boxes than this -> 'crowded'
CLASS_IDS = {"vehicle": 1, "human": 2}


def best_rtmdet_match(bbox, mapped_class, rtmdet_boxes, class_map):
    """Best-IoU RTMDet box whose mapped class equals mapped_class."""
    best = 0.0
    for det in rtmdet_boxes:
        if class_map.get(det["label"].lower()) != mapped_class:
            continue
        best = max(best, iou(bbox, det["bbox"]))
    return best


def route_frame(img, gdino, rtmdet, class_map):
    """-> (accepted[], candidates[]) annotation records for one frame."""
    accepted, candidates = [], []
    w, h = img["width"], img["height"]
    for det in gdino:
        mapped = class_map.get(det["label"].lower())
        if mapped is None:  # IGNORE
            continue
        bbox = clip_bbox(det["bbox"], w, h)
        if bbox is None:
            continue
        score = det["score"]
        if mapped in CLASS_IDS:
            cat = CLASS_IDS[mapped]
            match_iou = best_rtmdet_match(bbox, mapped, rtmdet, class_map)
            agree = match_iou >= CROSS_CHECK_IOU
            if score >= AUTO_ACCEPT_SCORE and agree:
                accepted.append(annotation_record(
                    img["id"], cat, bbox, score, "gdino+rtmdet", "auto_ok",
                    det["label"], {"rtmdet_iou": round(match_iou, 4), "agree": True}))
                continue
            reasons = set()
            if score < AUTO_ACCEPT_SCORE:
                reasons.add("low_conf")
            if not agree:
                reasons.add("models_disagree")
            if 10.0 <= max(bbox[2], bbox[3]) <= 40.0 and score < 0.6:
                reasons.add("small_box_low_conf")
            ann = annotation_record(
                img["id"], cat, bbox, score, "grounding_dino", "auto_ok",
                det["label"], {"rtmdet_iou": round(match_iou, 4), "agree": agree})
            ann["review_status"] = "needs_review"
            candidates.append((ann, reasons))
        else:  # REVIEW-routed label (e.g. "technical") — class itself unclear
            ann = annotation_record(img["id"], 1, bbox, score, "grounding_dino",
                                    "auto_ok", det["label"], None)
            ann["review_status"] = "needs_review"
            ann["category_id"] = None  # human picks; never packed unfixed
            candidates.append((ann, {"models_disagree"}))
    return accepted, candidates


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--budget", type=int, default=REVIEW_BUDGET_DEFAULT,
                    help="max review boxes handed to the human")
    args = ap.parse_args()

    root = dataset_root(args.root)
    class_map = {k.lower(): v for k, v in synonyms()["map"].items()
                 if v in ("vehicle", "human", "REVIEW")}
    class_map = {k: (v if v in ("vehicle", "human") else "REVIEW")
                 for k, v in class_map.items()}

    images, annotations = [], []
    queue_items = []
    frame_drops, rescan_requests = [], {}
    n_auto = n_candidates = 0
    videos_seen, videos_unlabeled = 0, []

    for vdir in sorted((root / "frames").iterdir()):
        vid = vdir.name
        images_json = vdir / "images.json"
        gdino_path = root / "raw_labels" / f"{vid}.gdino.json"
        rtmdet_path = root / "raw_labels" / f"{vid}.rtmdet.json"
        if not images_json.exists():
            continue
        videos_seen += 1
        if not gdino_path.exists():
            videos_unlabeled.append(vid)
            continue
        gdino = read_json(gdino_path)["frames"]
        rtmdet = read_json(rtmdet_path)["frames"] if rtmdet_path.exists() else {}

        kept_any = False
        for img in read_json(images_json)["images"]:
            name = img["file_name"]
            graw = gdino.get(name, {"boxes": []})
            rraw = rtmdet.get(name, {"boxes": []})

            if img["extraction_mode"] == "terminal" and not graw["boxes"] \
                    and not graw.get("rescanned"):
                rescan_requests.setdefault(vid, []).append(name)
                continue  # decided on the next merge run, after the rescan

            accepted, candidates = route_frame(img, graw["boxes"], rraw["boxes"],
                                               class_map)
            n_auto += len(accepted)
            n_candidates += len(candidates)

            reasons_frame = set()
            if img["extraction_mode"] == "terminal" and not accepted and not candidates:
                # rescanned and still empty: mostly garbage, a few worth eyes
                reasons_frame.add("terminal_no_labels")
            if len(accepted) + len(candidates) > CROWDED_BOXES:
                reasons_frame.add("crowded")

            if not accepted and not candidates and not reasons_frame:
                frame_drops.append({"source_video_id": vid,
                                    "timestamp_ms": img["timestamp_ms"],
                                    "extraction_mode": img["extraction_mode"],
                                    "reason_code": "EMPTY_GARBAGE",
                                    "detail": "no detections"})
                continue

            if candidates or reasons_frame:
                reasons = sorted(reasons_frame |
                                 {r for _, rs in candidates for r in rs})
                item = {
                    "image_id": img["id"], "file_name": name,
                    "source_video_id": vid, "timestamp_ms": img["timestamp_ms"],
                    "extraction_mode": img["extraction_mode"],
                    "reasons": reasons or ["low_conf"],
                    "boxes": [{"annotation_id": a["id"], "bbox": a["bbox"],
                               "proposed_category_id": a["category_id"],
                               "score": a["score"], "raw_label": a["raw_label"]}
                              for a, _ in candidates],
                    "suggested_action": ("confirm empty or draw the missed target"
                                         if "terminal_no_labels" in reasons_frame
                                         else "accept / fix / reject each box"),
                }
                item["toughness"] = toughness(item)
                queue_items.append(item)

            images.append(img)
            annotations.extend(accepted)
            annotations.extend(a for a, _ in candidates)
            kept_any = True

        if not kept_any and vid not in rescan_requests:
            ledger.exclude_video(root, vid, "NO_LABELS",
                                 detail="no frame kept any label", video_id=vid)

    # rescan requests block those frames until ds_label.py --rescan runs
    for vid, names in rescan_requests.items():
        write_json(root / "raw_labels" / f"{vid}.rescan_request.json",
                   {"video_id": vid, "frames": sorted(names)})

    kept, overflow = cap_queue(queue_items, args.budget)
    kept_ids = {i["image_id"] for i in kept}
    overflow_ann_ids = {b["annotation_id"] for i in overflow for b in i["boxes"]}
    annotations = [a for a in annotations
                   if not (a["review_status"] == "needs_review"
                           and a["id"] in overflow_ann_ids)]
    still_annotated = {a["image_id"] for a in annotations}
    final_images = []
    for img in images:
        if img["id"] in still_annotated or img["id"] in kept_ids:
            final_images.append(img)
        else:
            frame_drops.append({"source_video_id": img["source_video_id"],
                                "timestamp_ms": img["timestamp_ms"],
                                "extraction_mode": img["extraction_mode"],
                                "reason_code": "REVIEW_OVERFLOW_DROP",
                                "detail": "all boxes past review budget"})

    if frame_drops:
        ledger.exclude_frames(root, frame_drops)

    write_json(root / "annotations" / "merged_labels.json", {
        "spec_version": SPEC_VERSION,
        "review_budget": args.budget,
        "images": final_images,
        "annotations": annotations,
    })
    queue = {"spec_version": SPEC_VERSION, "budget": args.budget,
             "queued": len(kept), "overflow_dropped": len(overflow), "items": kept}
    validate(queue, "review_queue")
    write_json(root / "review" / "review_queue.json", queue)

    print(f"videos: {videos_seen} extracted, {len(videos_unlabeled)} awaiting labels"
          + (f" ({', '.join(videos_unlabeled[:5])}...)" if videos_unlabeled else ""))
    print(f"boxes: {n_auto} auto-accepted, {n_candidates} review candidates -> "
          f"{len(kept)} frames queued (budget {args.budget}), "
          f"{len(overflow)} overflow-dropped")
    print(f"frames: {len(final_images)} kept, {len(frame_drops)} dropped (ledgered)")
    if rescan_requests:
        n = sum(len(v) for v in rescan_requests.values())
        print(f"RESCAN NEEDED: {n} terminal frames in {len(rescan_requests)} videos "
              f"— run ds_label.py --rescan, then ds_merge.py again")


if __name__ == "__main__":
    main()
