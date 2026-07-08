#!/usr/bin/env python3
"""THE gate — validates a built dataset end to end. The training pipeline
runs this before touching the data; any drift between manifest and artifacts,
any schema violation, any broken invariant fails loudly.

Checks: JSON-Schema validity of every artifact · split disjointness/coverage
· annotation->image references · bbox bounds · category ids · review budget
· manifest content hashes recomputed · used/indexed/exclusions arithmetic ·
frame files exist on disk (sha256 spot-check; --deep rehashes every frame).

  ds_validate.py --root <DATASET_ROOT> [--deep]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, read_json
from airpoc_ds.hashing import sha256_file
from airpoc_ds.schema import validate as schema_validate

RESULTS = []


def check(name, fn):
    try:
        fn()
        RESULTS.append((True, name))
        print(f"  PASS  {name}")
    except Exception as e:  # noqa: BLE001 - each failure reported, all checks run
        RESULTS.append((False, name))
        print(f"  FAIL  {name}: {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--deep", action="store_true",
                    help="rehash every frame PNG (slow)")
    args = ap.parse_args()
    root = dataset_root(args.root)

    manifest = read_json(root / "manifest.json")
    queue = read_json(root / "review" / "review_queue.json")
    split_manifest = read_json(root / "splits" / "split_manifest.json")
    exclusions = read_json(root / "exclusions" / "exclusions.json")
    snapshot = read_json(root / "catalog" / "catalog_snapshot.json")
    cocos = {s: read_json(root / "annotations" / f"instances_{s}.json")
             for s in ("train", "val", "test")}

    check("schema: manifest", lambda: schema_validate(manifest, "manifest"))
    check("schema: review_queue", lambda: schema_validate(queue, "review_queue"))
    check("schema: split_manifest", lambda: schema_validate(split_manifest, "split_manifest"))
    check("schema: exclusions", lambda: schema_validate(exclusions, "exclusions"))
    for s, coco in cocos.items():
        check(f"schema: instances_{s}", lambda c=coco: schema_validate(c, "coco_instances"))

    def split_invariants():
        seen = {}
        for s, coco in cocos.items():
            for img in coco["images"]:
                vid = img["source_video_id"]
                assert split_manifest["assignments"].get(vid) == s, \
                    f"{vid} packed into {s} but assigned {split_manifest['assignments'].get(vid)}"
                assert seen.setdefault(vid, s) == s, f"video {vid} in two splits"
                assert img["split"] == s, f"image {img['id']} split field mismatch"
    check("invariant: split by video, no leaks", split_invariants)

    def references():
        for s, coco in cocos.items():
            ids = {i["id"] for i in coco["images"]}
            assert len(ids) == len(coco["images"]), f"{s}: duplicate image ids"
            dims = {i["id"]: (i["width"], i["height"]) for i in coco["images"]}
            aids = [a["id"] for a in coco["annotations"]]
            assert len(aids) == len(set(aids)), f"{s}: duplicate annotation ids"
            for a in coco["annotations"]:
                assert a["image_id"] in ids, f"{s}: orphan annotation {a['id']}"
                w, h = dims[a["image_id"]]
                x, y, bw, bh = a["bbox"]
                assert x >= 0 and y >= 0 and x + bw <= w + 0.5 and y + bh <= h + 0.5, \
                    f"{s}: bbox out of bounds on annotation {a['id']}"
                assert a["category_id"] in (1, 2)
    check("invariant: references, bounds, categories", references)

    check("invariant: review budget respected",
          lambda: (_ for _ in ()).throw(AssertionError(
              f"{queue['queued']} > {queue['budget']}"))
          if queue["queued"] > queue["budget"] else None)

    def hashes():
        for key, path in (
                ("instances_train_sha256", root / "annotations" / "instances_train.json"),
                ("instances_val_sha256", root / "annotations" / "instances_val.json"),
                ("instances_test_sha256", root / "annotations" / "instances_test.json"),
                ("split_manifest_sha256", root / "splits" / "split_manifest.json"),
                ("stats_sha256", root / "reports" / "stats.json")):
            actual = sha256_file(path)
            assert actual == manifest["hashes"][key], f"{key} drifted"
    check("manifest: content hashes", hashes)

    def arithmetic():
        indexed = len(snapshot["videos"])
        used = len({i["source_video_id"] for c in cocos.values()
                    for i in c["images"]})
        excluded = len({v["stem"] for v in exclusions["videos"]})
        assert indexed - used == excluded, \
            f"indexed({indexed}) - used({used}) != exclusions({excluded})"
        assert manifest["source"]["videos_used"] == used
        assert manifest["exclusions_count"] == excluded
    check("arithmetic: indexed - used == exclusions", arithmetic)

    def frames_exist():
        import random
        rng = random.Random(0)
        all_imgs = [i for c in cocos.values() for i in c["images"]]
        sample = all_imgs if args.deep else rng.sample(all_imgs,
                                                       min(20, len(all_imgs)))
        for img in all_imgs:
            p = root / "frames" / img["source_video_id"] / img["file_name"]
            assert p.exists(), f"missing frame file {img['file_name']}"
        for img in sample:
            p = root / "frames" / img["source_video_id"] / img["file_name"]
            assert sha256_file(p) == img["frame_sha256"], \
                f"frame hash drifted: {img['file_name']}"
    check("frames: files exist" + (" + full rehash" if args.deep else " + spot rehash"),
          frames_exist)

    failed = sum(1 for ok, _ in RESULTS if not ok)
    print(f"{len(RESULTS) - failed}/{len(RESULTS)} checks passed")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
