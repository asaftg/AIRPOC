#!/usr/bin/env python3
"""GroundingDINO auto-labeling (primary, open-vocab) — GPU stage.

Runs heavy tiled + multi-scale inference (SPEC.md §5) over every extracted
frame and writes raw_labels/<video_id>.gdino.json. Raw output only — class
mapping, cross-checking, and accept/review routing happen in ds_merge.py.

--rescan services raw_labels/<video_id>.rescan_request.json written by
ds_merge.py: terminal frames with zero detections get one extra-zoomed pass
(400 px tiles at 2x/3x) before being declared empty.

Model: IDEA-Research/grounding-dino-base via HuggingFace transformers
(Apache-2.0; NOT Ultralytics YOLO). Needs torch + transformers + GPU.

  ds_label.py --root <DATASET_ROOT> [--only VIDEO_ID] [--force] [--rescan]
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, read_json, write_json
from airpoc_ds.schema import synonyms
from airpoc_ds.tiling import (RESCAN_SCALES, RESCAN_TILE_PX, SCALES, TILE_OVERLAP,
                              TILE_PX, nms_per_label, run_tiled)

MODEL_ID = "IDEA-Research/grounding-dino-base"
BOX_THRESHOLD = 0.25
TEXT_THRESHOLD = 0.25


def load_model():
    try:
        import torch
        from transformers import (AutoModelForZeroShotObjectDetection,
                                  AutoProcessor)
    except ImportError as e:
        sys.exit(f"GPU stage needs torch + transformers ({e}); "
                 "see tools/requirements.txt [gpu] section")
    device = "cuda" if torch.cuda.is_available() else "cpu"
    if device == "cpu":
        print("WARNING: no CUDA — this will be very slow", file=sys.stderr)
    processor = AutoProcessor.from_pretrained(MODEL_ID)
    model = AutoModelForZeroShotObjectDetection.from_pretrained(MODEL_ID).to(device)
    model.eval()
    return model, processor, device


def make_infer(model, processor, device, prompt):
    import torch

    def infer(crop: np.ndarray):
        rgb = np.stack([crop] * 3, axis=-1) if crop.ndim == 2 else crop
        pil = Image.fromarray(rgb)
        inputs = processor(images=pil, text=prompt, return_tensors="pt").to(device)
        with torch.no_grad():
            outputs = model(**inputs)
        res = processor.post_process_grounded_object_detection(
            outputs, inputs.input_ids, threshold=BOX_THRESHOLD,
            text_threshold=TEXT_THRESHOLD, target_sizes=[pil.size[::-1]])[0]
        dets = []
        for box, score, label in zip(res["boxes"], res["scores"], res["text_labels"]):
            x0, y0, x1, y1 = box.tolist()
            dets.append({"bbox": [x0, y0, x1 - x0, y1 - y0],
                         "score": float(score), "label": label})
        return dets

    return infer


def label_frames(png_paths, infer, tile, scales):
    out = {}
    for p in png_paths:
        gray = np.asarray(Image.open(p).convert("L"))
        dets = nms_per_label(run_tiled(gray, infer, scales=scales, tile=tile,
                                       overlap=TILE_OVERLAP))
        out[p.name] = {"boxes": dets}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--only", help="label a single video_id")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--rescan", action="store_true",
                    help="service ds_merge rescan requests (harder tiles)")
    args = ap.parse_args()

    root = dataset_root(args.root)
    prompt = synonyms()["gdino_prompt"]
    model, processor, device = load_model()
    infer = make_infer(model, processor, device, prompt)

    for vdir in sorted((root / "frames").iterdir()):
        vid = vdir.name
        if args.only and vid != args.only:
            continue
        out_path = root / "raw_labels" / f"{vid}.gdino.json"

        if args.rescan:
            req_path = root / "raw_labels" / f"{vid}.rescan_request.json"
            if not req_path.exists() or not out_path.exists():
                continue
            raw = read_json(out_path)
            names = read_json(req_path)["frames"]
            frames = label_frames([vdir / n for n in names], infer,
                                  RESCAN_TILE_PX, RESCAN_SCALES)
            for name, res in frames.items():
                res["rescanned"] = True
                raw["frames"][name] = res
            write_json(out_path, raw)
            req_path.unlink()
            print(f"{vid}: rescanned {len(names)} terminal frames")
            continue

        if out_path.exists() and not args.force:
            continue
        pngs = sorted(vdir.glob("*.png"))
        if not pngs:
            continue
        frames = label_frames(pngs, infer, TILE_PX, SCALES)
        write_json(out_path, {
            "video_id": vid, "model": "grounding_dino", "model_id": MODEL_ID,
            "params": {"tile_px": TILE_PX, "overlap": TILE_OVERLAP,
                       "scales": list(SCALES), "box_threshold": BOX_THRESHOLD,
                       "text_threshold": TEXT_THRESHOLD, "prompt": prompt},
            "frames": frames,
        })
        n = sum(len(f["boxes"]) for f in frames.values())
        print(f"{vid}: {len(frames)} frames, {n} raw boxes")


if __name__ == "__main__":
    main()
