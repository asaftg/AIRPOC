"""Tiled + multi-scale (SAHI-style) inference helpers, shared by both
labelers (SPEC.md §5). Pure geometry — the models plug in as callables.

Accuracy over speed by design: the build machine has weeks, and heavy tiling
is the main lever that keeps the human review pile small.
"""

# frozen tiling parameters (SPEC.md §5)
TILE_PX = 800
TILE_OVERLAP = 0.25
SCALES = (1.0, 1.5, 2.0)
NMS_IOU = 0.5
# terminal re-scan: extra-zoomed pass for terminal frames with zero detections
RESCAN_TILE_PX = 400
RESCAN_SCALES = (2.0, 3.0)

from .coco import iou


def tile_grid(width, height, tile=TILE_PX, overlap=TILE_OVERLAP):
    """-> [(x, y, w, h)] tiles covering the image with the given overlap.
    Images smaller than a tile yield the full frame once."""
    step = max(1, int(tile * (1.0 - overlap)))
    xs = list(range(0, max(1, width - tile) + 1, step)) if width > tile else [0]
    ys = list(range(0, max(1, height - tile) + 1, step)) if height > tile else [0]
    if width > tile and xs[-1] + tile < width:
        xs.append(width - tile)
    if height > tile and ys[-1] + tile < height:
        ys.append(height - tile)
    return [(x, y, min(tile, width - x), min(tile, height - y))
            for y in ys for x in xs]


def run_tiled(image, infer, scales=SCALES, tile=TILE_PX, overlap=TILE_OVERLAP):
    """Run `infer(crop) -> [{bbox:[x,y,w,h], score, label}]` over every tile at
    every scale; boxes come back in ORIGINAL image coordinates. `image` is an
    HxW (mono) or HxWx3 numpy array; scaling uses PIL bilinear."""
    import numpy as np
    from PIL import Image

    h0, w0 = image.shape[:2]
    out = []
    for scale in scales:
        if scale == 1.0:
            scaled = image
        else:
            pil = Image.fromarray(image)
            scaled = np.asarray(pil.resize((int(w0 * scale), int(h0 * scale)),
                                           Image.BILINEAR))
        sh, sw = scaled.shape[:2]
        for tx, ty, tw, th in tile_grid(sw, sh, tile, overlap):
            crop = scaled[ty:ty + th, tx:tx + tw]
            for det in infer(crop):
                x, y, w, h = det["bbox"]
                out.append({
                    "bbox": [(tx + x) / scale, (ty + y) / scale,
                             w / scale, h / scale],
                    "score": float(det["score"]),
                    "label": det["label"],
                })
    return out


def nms_per_label(dets, iou_thr=NMS_IOU):
    """Greedy class-wise NMS on [{bbox, score, label}] (highest score wins)."""
    kept = []
    for label in sorted({d["label"] for d in dets}):
        pool = sorted((d for d in dets if d["label"] == label),
                      key=lambda d: -d["score"])
        chosen = []
        for d in pool:
            if all(iou(d["bbox"], c["bbox"]) < iou_thr for c in chosen):
                chosen.append(d)
        kept.extend(chosen)
    return kept
