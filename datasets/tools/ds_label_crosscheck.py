#!/usr/bin/env python3
"""RTMDet cross-check labeling (independent second opinion) — GPU stage.

Same tiled + multi-scale sweep as ds_label.py but with RTMDet (MMDetection,
Apache-2.0, COCO vocabulary). ds_merge.py auto-accepts a GroundingDINO box
only when an RTMDet box of the same mapped class agrees (IoU >= 0.5) —
two independent architectures agreeing is what keeps the human pile small.

Writes raw_labels/<video_id>.rtmdet.json. Needs torch + mmdet + mmcv + GPU.

  ds_label_crosscheck.py --root <DATASET_ROOT> [--only VIDEO_ID] [--force]
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, write_json
from airpoc_ds.tiling import SCALES, TILE_OVERLAP, TILE_PX, nms_per_label, run_tiled

MODEL = "rtmdet_l_8xb32-300e_coco"  # large: sized to the 16 GB A4000, accuracy first
SCORE_THRESHOLD = 0.20


def load_model():
    try:
        from mmdet.apis import DetInferencer
    except ImportError as e:
        sys.exit(f"GPU stage needs mmdet + mmcv ({e}); "
                 "see tools/requirements.txt [gpu] section")
    return DetInferencer(model=MODEL, show_progress=False)


def make_infer(inferencer):
    classes = list(inferencer.model.dataset_meta["classes"])

    def infer(crop: np.ndarray):
        rgb = np.stack([crop] * 3, axis=-1) if crop.ndim == 2 else crop
        pred = inferencer(rgb[..., ::-1], return_datasamples=False)["predictions"][0]
        dets = []
        for bbox, label_i, score in zip(pred["bboxes"], pred["labels"], pred["scores"]):
            if score < SCORE_THRESHOLD:
                continue
            x0, y0, x1, y1 = bbox
            dets.append({"bbox": [x0, y0, x1 - x0, y1 - y0],
                         "score": float(score), "label": classes[label_i]})
        return dets

    return infer


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--only", help="label a single video_id")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    root = dataset_root(args.root)
    infer = make_infer(load_model())

    for vdir in sorted((root / "frames").iterdir()):
        vid = vdir.name
        if args.only and vid != args.only:
            continue
        out_path = root / "raw_labels" / f"{vid}.rtmdet.json"
        if out_path.exists() and not args.force:
            continue
        pngs = sorted(vdir.glob("*.png"))
        if not pngs:
            continue
        frames = {}
        for p in pngs:
            gray = np.asarray(Image.open(p).convert("L"))
            dets = nms_per_label(run_tiled(gray, infer, scales=SCALES,
                                           tile=TILE_PX, overlap=TILE_OVERLAP))
            frames[p.name] = {"boxes": dets}
        write_json(out_path, {
            "video_id": vid, "model": "rtmdet", "model_id": MODEL,
            "params": {"tile_px": TILE_PX, "overlap": TILE_OVERLAP,
                       "scales": list(SCALES), "score_threshold": SCORE_THRESHOLD},
            "frames": frames,
        })
        n = sum(len(f["boxes"]) for f in frames.values())
        print(f"{vid}: {len(frames)} frames, {n} raw boxes")


if __name__ == "__main__":
    main()
