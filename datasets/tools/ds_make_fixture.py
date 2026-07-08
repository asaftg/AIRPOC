#!/usr/bin/env python3
"""Synthesize a tiny offline fixture DATASET_ROOT — no CDN, no GPU, no ffmpeg.

Builds what ds_download/ds_probe/ds_extract/ds_label would have produced for
three fake videos (gray frames with bright rectangles; a growing rectangle
plays the diving target) plus stub GroundingDINO/RTMDet outputs covering every
routing path: agreement (auto-accept), low confidence, model disagreement,
REVIEW-routed label, IGNORE label, crowded frame, empty garbage, terminal
zero-detection after rescan, a replay frame, a looming-fallback video, and a
failed download for the exclusions arithmetic.

Then the whole non-GPU spine runs against it:
  ds_merge.py -> ds_split.py -> ds_pack.py -> ds_stats.py -> ds_manifest.py
  -> ds_validate.py

  ds_make_fixture.py --root <empty dir>
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import SPEC_VERSION, ensure_layout, write_json
from airpoc_ds import ledger
from airpoc_ds.coco import image_record
from airpoc_ds.hashing import sha256_bytes, video_id
from airpoc_ds.terminal import sample_plan  # noqa: F401 (fixture mirrors its output shape)

W, H = 320, 240


def draw(rects):
    """Gray frame with bright rects: [(x, y, w, h, value)]."""
    img = np.full((H, W), 60, dtype=np.uint8)
    for x, y, w, h, v in rects:
        img[y:y + h, x:x + w] = v
    return img


def box(x, y, w, h, score, label):
    return {"bbox": [float(x), float(y), float(w), float(h)],
            "score": score, "label": label}


class VideoBuilder:
    def __init__(self, root, stem, duration_s, segments, annotated=True,
                 terminal_source="segments"):
        self.root, self.stem = root, stem
        self.vid = video_id(stem, sha256_bytes(stem.encode()))
        self.duration_s = duration_s
        self.segments = segments
        self.annotated = annotated
        self.terminal_source = terminal_source
        self.images, self.gdino, self.rtmdet = [], {}, {}
        self.probe = {"duration_s": duration_s, "fps": 30.0, "width": W,
                      "height": H, "pix_fmt": "yuv420p", "color_range": "tv",
                      "codec": "h264", "stem": stem,
                      "sha256": sha256_bytes(stem.encode()),
                      "url": f"https://example.invalid/videos/{stem}.mp4"}

    def frame(self, t_ms, mode, rects, gdino_boxes, rtmdet_boxes,
              from_replay=False, rescanned=False):
        out_dir = self.root / "frames" / self.vid
        out_dir.mkdir(parents=True, exist_ok=True)
        name = f"{self.vid}_{t_ms:08d}_{mode}.png"
        p = out_dir / name
        Image.fromarray(draw(rects)).save(p, optimize=True)
        img = image_record(self.vid, t_ms, mode, W, H, self.probe, "bt601_full",
                           sha256_bytes(p.read_bytes()), from_replay=from_replay,
                           terminal_source=self.terminal_source)
        img["frame_index"] = len(self.images)
        self.images.append(img)
        g = {"boxes": gdino_boxes}
        if rescanned:
            g["rescanned"] = True
        self.gdino[name] = g
        self.rtmdet[name] = {"boxes": rtmdet_boxes}

    def finish(self):
        write_json(self.root / "frames" / self.vid / "images.json", {
            "video_id": self.vid,
            "terminal_source": self.terminal_source,
            "extraction_params": {"baseline_fps": 2.0, "terminal_fps": 15.0,
                                  "terminal_tail_s": 6.0},
            "dedup_dropped": 1,
            "images": self.images,
        })
        write_json(self.root / "raw_labels" / f"{self.vid}.gdino.json", {
            "video_id": self.vid, "model": "grounding_dino",
            "model_id": "fixture-stub", "params": {}, "frames": self.gdino})
        write_json(self.root / "raw_labels" / f"{self.vid}.rtmdet.json", {
            "video_id": self.vid, "model": "rtmdet",
            "model_id": "fixture-stub", "params": {}, "frames": self.rtmdet})
        return self


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", required=True, help="empty dir to build the fixture in")
    args = ap.parse_args()
    root = ensure_layout(Path(args.root))

    # --- vidA: vehicles; every accept/review/ignore path -------------------
    a = VideoBuilder(root, "2026-01-01_convoy_on_coastal_road", 30.0, [
        {"time": 0.0, "type": "banner_start"},
        {"time": 2.0, "type": "flight_start"},
        {"time": 20.0, "type": "replay_start"},
        {"time": 28.0, "type": "pause_start"},
    ])
    car = (100, 80, 40, 24, 200)
    a.frame(2000, "b", [car], [box(*car[:4], 0.82, "car")],
            [box(98, 79, 43, 25, 0.9, "car")])                      # auto-accept
    a.frame(2500, "b", [car], [box(*car[:4], 0.30, "truck")],
            [box(*car[:4], 0.8, "truck")])                          # low_conf
    a.frame(3000, "b", [car], [box(*car[:4], 0.70, "technical")], [])  # REVIEW route
    a.frame(3500, "b", [], [box(10, 10, 30, 30, 0.9, "tree")], [])     # IGNORE -> empty garbage
    for i, t in enumerate((19500, 19700, 19900)):                   # the dive: 12->28 px
        s = 12 + 8 * i
        r = (150 - s // 2, 110 - s // 2, s, s, 220)
        a.frame(t, "t", [r], [box(*r[:4], 0.85, "car")], [box(*r[:4], 0.8, "car")])
    a.frame(21000, "b", [car], [box(*car[:4], 0.55, "car")], [],
            from_replay=True)                                       # models_disagree
    a.finish()

    # --- vidB: humans; crowded frame --------------------------------------
    b = VideoBuilder(root, "2026-02-02_squad_crossing_field", 20.0, [
        {"time": 1.0, "type": "flight_start"},
    ])
    crowd = [(20 + 30 * i, 100, 8, 20, 190) for i in range(9)]
    b.frame(1000, "b", crowd,
            [box(*r[:4], 0.8, "person") for r in crowd],
            [box(*r[:4], 0.85, "person") for r in crowd])           # 9 auto + crowded
    lone = (200, 90, 10, 24, 210)
    b.frame(1500, "b", [lone], [box(*lone[:4], 0.9, "person")],
            [box(*lone[:4], 0.9, "person")])                        # auto-accept
    h1 = (160, 100, 9, 20, 210)
    b.frame(17500, "t", [h1], [box(*h1[:4], 0.85, "human")],
            [box(*h1[:4], 0.8, "person")])                          # auto, crux band
    h2 = (162, 98, 10, 22, 210)
    b.frame(17700, "t", [h2], [box(*h2[:4], 0.50, "human")],
            [box(*h2[:4], 0.7, "person")])                          # small_box_low_conf
    b.finish()

    # --- vidC: no annotations -> looming fallback; rescan-still-empty ------
    c = VideoBuilder(root, "2026-03-03_unlabeled_clip", 15.0, [],
                     annotated=False, terminal_source="looming_heuristic")
    c.frame(0, "b", [], [], [])                                     # empty garbage
    v = (60, 60, 50, 30, 200)
    c.frame(500, "b", [v], [box(*v[:4], 0.95, "car")], [box(*v[:4], 0.9, "car")])
    c.frame(9500, "t", [], [], [], rescanned=True)                  # terminal_no_labels
    v2 = (140, 100, 30, 18, 220)
    c.frame(9700, "t", [v2], [box(*v2[:4], 0.9, "car")], [box(*v2[:4], 0.9, "car")])
    c.finish()

    # --- catalog snapshot + mirror index + one failed download -------------
    builders = (a, b, c)
    failed_stem = "2026-04-04_unreachable_video"
    videos = []
    for vb in builders:
        videos.append({"stem": vb.stem, "date": vb.stem[:10], "description": "fixture",
                       "town": "fixture", "lat": "0", "lon": "0", "status": "",
                       "video_url": vb.probe["url"], "thumbnail_url": "",
                       "annotated": vb.annotated, "segments": vb.segments,
                       "exclusion_masks": []})
    videos.append({"stem": failed_stem, "date": "2026-04-04", "description": "fixture",
                   "town": "fixture", "lat": "0", "lon": "0", "status": "",
                   "video_url": f"https://example.invalid/videos/{failed_stem}.mp4",
                   "thumbnail_url": "", "annotated": False, "segments": [],
                   "exclusion_masks": []})
    write_json(root / "catalog" / "catalog_snapshot.json", {
        "spec_version": SPEC_VERSION,
        "catalog_repo": "fixture",
        "catalog_commit": "0" * 40,
        "fetched_utc": "2026-01-01T00:00:00Z",
        "videos": videos, "annotation_orphans": [], "cdn_probe": [],
    })
    write_json(root / "videos" / "index.json",
               {vb.stem: {"video_id": vb.vid, "sha256": vb.probe["sha256"],
                          "url": vb.probe["url"], "bytes": 0} for vb in builders})
    for vb in builders:
        write_json(root / "videos" / f"{vb.vid}.probe.json", vb.probe)
    ledger.exclude_video(root, failed_stem, "DOWNLOAD_FAIL",
                         detail="fixture: connection refused")

    print(f"fixture built at {root} — now run:")
    print("  ds_merge.py -> ds_split.py -> ds_pack.py --pending=drop "
          "-> ds_stats.py -> ds_manifest.py --version 0.0.1-fixture -> ds_validate.py")
    print(f"video_ids: {', '.join(vb.vid for vb in builders)}")


if __name__ == "__main__":
    main()
