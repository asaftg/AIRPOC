#!/usr/bin/env python3
"""Dataset statistics: class counts + the box-size story (SPEC.md §7).

Box sizes are reported twice: as-is in source pixels, and rescaled to the
seeker's 1088-row frame ("seeker-referenced", side * 1088/frame_height) —
this footage is assorted 720p/1080p, not our geometry, so the rescale is
labelled, never silently applied. The 10-40 px band is the detector's crux
and gets called out explicitly.

Writes reports/stats.{md,json} (+ .png histogram if matplotlib is present).

  ds_stats.py --root <DATASET_ROOT>
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, read_json, write_json

BINS = [(0, 10), (10, 20), (20, 30), (30, 40), (40, 60), (60, 100),
        (100, 200), (200, 400), (400, 10**9)]
CLASS_NAMES = {1: "vehicle", 2: "human"}


def bin_label(lo, hi):
    return f"{lo}-{hi}px" if hi < 10**9 else f"{lo}px+"


def histogram(sides):
    out = {bin_label(*b): 0 for b in BINS}
    for s in sides:
        for lo, hi in BINS:
            if lo <= s < hi:
                out[bin_label(lo, hi)] += 1
                break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    args = ap.parse_args()
    root = dataset_root(args.root)

    images, annotations = [], []
    per_split = {}
    for split in ("train", "val", "test"):
        coco = read_json(root / "annotations" / f"instances_{split}.json")
        images += coco["images"]
        annotations += coco["annotations"]
        per_split[split] = {"images": len(coco["images"]),
                            "annotations": len(coco["annotations"])}

    img_h = {i["id"]: i["height"] for i in images}
    sides, seeker_sides = {1: [], 2: []}, {1: [], 2: []}
    for a in annotations:
        side = a["bbox_max_side_px"]
        sides[a["category_id"]].append(side)
        seeker_sides[a["category_id"]].append(side * 1088.0 / img_h[a["image_id"]])

    def band(vals):
        return sum(1 for s in vals if 10.0 <= s <= 40.0)

    stats = {
        "images": len(images),
        "annotations": len(annotations),
        "per_split": per_split,
        "by_class": {CLASS_NAMES[c]: len(v) for c, v in sides.items()},
        "by_mode": {m: sum(1 for i in images if i["extraction_mode"] == m)
                    for m in ("baseline", "terminal")},
        "from_replay": sum(1 for i in images if i["from_replay"]),
        "small_10_40px": {CLASS_NAMES[c]: band(v) for c, v in sides.items()},
        "small_10_40px_seeker_ref": {CLASS_NAMES[c]: band(v)
                                     for c, v in seeker_sides.items()},
        "hist_source_px": {CLASS_NAMES[c]: histogram(v) for c, v in sides.items()},
        "hist_seeker_ref": {CLASS_NAMES[c]: histogram(v)
                            for c, v in seeker_sides.items()},
    }
    write_json(root / "reports" / "stats.json", stats)

    lines = ["# Dataset stats", "",
             f"{stats['images']} images · {stats['annotations']} boxes · "
             f"vehicle {stats['by_class']['vehicle']} / human {stats['by_class']['human']} · "
             f"terminal {stats['by_mode']['terminal']} / baseline {stats['by_mode']['baseline']} · "
             f"{stats['from_replay']} from replays", "",
             "**Crux band (10-40 px boxes):** "
             f"source-px vehicle {stats['small_10_40px']['vehicle']} / human "
             f"{stats['small_10_40px']['human']}; seeker-referenced (x1088/h) vehicle "
             f"{stats['small_10_40px_seeker_ref']['vehicle']} / human "
             f"{stats['small_10_40px_seeker_ref']['human']}", ""]
    for title, hist in (("Source pixels", stats["hist_source_px"]),
                        ("Seeker-referenced (rescaled to 1088 rows)",
                         stats["hist_seeker_ref"])):
        lines += [f"## Box max-side histogram — {title}", "",
                  "| band | vehicle | human |", "|---|---|---|"]
        for b in BINS:
            k = bin_label(*b)
            lines.append(f"| {k} | {hist['vehicle'][k]} | {hist['human'][k]} |")
        lines.append("")
    (root / "reports" / "stats.md").write_text("\n".join(lines), encoding="utf-8")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(1, 2, figsize=(12, 4))
        for ax, (title, hist) in zip(axes, (("source px", stats["hist_source_px"]),
                                            ("seeker-ref", stats["hist_seeker_ref"]))):
            labels = [bin_label(*b) for b in BINS]
            x = range(len(labels))
            ax.bar([i - 0.2 for i in x], [hist["vehicle"][k] for k in labels],
                   width=0.4, label="vehicle")
            ax.bar([i + 0.2 for i in x], [hist["human"][k] for k in labels],
                   width=0.4, label="human")
            ax.set_xticks(list(x), labels, rotation=45, ha="right")
            ax.set_title(f"box max side — {title}")
            ax.axvspan(0.5, 3.5, alpha=0.15, color="red", label="10-40 crux")
            ax.legend()
        fig.tight_layout()
        fig.savefig(root / "reports" / "stats.png", dpi=120)
    except ImportError:
        pass

    print(f"wrote reports/stats.md + .json ({stats['images']} images, "
          f"{stats['annotations']} boxes)")


if __name__ == "__main__":
    main()
