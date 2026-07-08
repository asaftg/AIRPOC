"""COCO record builders + box math (SPEC.md §2, schema/coco_instances.schema.json)."""

from . import SPEC_VERSION
from .hashing import annotation_int_id, image_int_id

CATEGORIES = [
    {"id": 1, "name": "vehicle", "supercategory": "object"},
    {"id": 2, "name": "human", "supercategory": "object"},
]


def iou(a, b):
    """IoU of two [x, y, w, h] boxes."""
    ax2, ay2 = a[0] + a[2], a[1] + a[3]
    bx2, by2 = b[0] + b[2], b[1] + b[3]
    ix = max(0.0, min(ax2, bx2) - max(a[0], b[0]))
    iy = max(0.0, min(ay2, by2) - max(a[1], b[1]))
    inter = ix * iy
    union = a[2] * a[3] + b[2] * b[3] - inter
    return inter / union if union > 0 else 0.0


def clip_bbox(bbox, width, height):
    """Clip [x,y,w,h] into image bounds; returns None if nothing remains."""
    x = min(max(bbox[0], 0.0), float(width))
    y = min(max(bbox[1], 0.0), float(height))
    w = min(bbox[2] - (x - bbox[0]), width - x)
    h = min(bbox[3] - (y - bbox[1]), height - y)
    if w <= 0 or h <= 0:
        return None
    return [round(x, 2), round(y, 2), round(w, 2), round(h, 2)]


def image_record(video_id, t_ms, mode, width, height, probe, luma_conversion,
                 frame_sha256, from_replay=False, terminal_source=None,
                 exclusion_masks=None):
    mode_name = "terminal" if mode == "t" else "baseline"
    rec = {
        "id": image_int_id(video_id, t_ms, mode),
        "file_name": f"{video_id}_{t_ms:08d}_{mode}.png",
        "width": width,
        "height": height,
        "source_video_id": video_id,
        "timestamp_ms": t_ms,
        "frame_index": 0,  # filled by the extractor (running index per video)
        "extraction_mode": mode_name,
        "from_replay": bool(from_replay),
        "luma_conversion": luma_conversion,
        "orig_resolution": [probe["width"], probe["height"]],
        "orig_pix_fmt": probe.get("pix_fmt", "unknown"),
        "orig_color_range": probe.get("color_range", "unknown"),
        "frame_sha256": frame_sha256,
        "split": "train",  # overwritten by ds_pack from the split manifest
    }
    if mode_name == "terminal":
        rec["terminal_source"] = terminal_source or "segments"
    if exclusion_masks:
        rec["exclusion_masks_applied"] = exclusion_masks
    return rec


def annotation_record(image_id, category_id, bbox, score, label_source,
                      review_status, raw_label, cross_check=None, iscrowd=0):
    return {
        "id": annotation_int_id(image_id, bbox, category_id, label_source),
        "image_id": image_id,
        "category_id": category_id,
        "bbox": [round(v, 2) for v in bbox],
        "area": round(bbox[2] * bbox[3], 2),
        "iscrowd": iscrowd,
        "score": round(float(score), 4),
        "label_source": label_source,
        "review_status": review_status,
        "raw_label": raw_label,
        "bbox_max_side_px": round(max(bbox[2], bbox[3]), 2),
        "cross_check": cross_check,
    }


def instances_skeleton(split):
    return {
        "info": {
            "spec_version": SPEC_VERSION,
            "split": split,
            "description": "AIRPOC vehicle/human bootstrap set from FPV strike footage",
        },
        "categories": CATEGORIES,
        "images": [],
        "annotations": [],
    }
