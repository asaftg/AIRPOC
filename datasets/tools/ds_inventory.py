#!/usr/bin/env python3
"""Aggregate catalog + mirror + probes + exclusions into the inventory memo
(reports/inventory.{md,json}) — the deliverable that states what we have and
under what terms, before any training use.

  ds_inventory.py --root <DATASET_ROOT>
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import USE_RESTRICTION, dataset_root, read_json, write_json
from airpoc_ds import ledger

# Verbatim from the catalog repo (pinned in the snapshot) — the rights facts.
LICENSE_QUOTE = (
    "LICENSE (CC0 1.0) waives rights to \"the metadata, catalog files, "
    "documentation, manifests, and other repository text/content\" and states: "
    "\"This waiver does not apply to third-party media files referenced by "
    "this repository, including the underlying video footage and thumbnails.\""
)
MEDIA_NOTICE_QUOTE = (
    "MEDIA_NOTICE.md: the videos/thumbnails are \"third-party media materials "
    "… provided for research, documentation, analysis, and archival indexing. "
    "No copyright license is granted here for the underlying third-party media "
    "content. Users are responsible for determining whether their use of the "
    "referenced media is permitted under applicable law.\""
)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    args = ap.parse_args()

    root = dataset_root(args.root)
    snapshot = read_json(root / "catalog" / "catalog_snapshot.json")
    index_path = root / "videos" / "index.json"
    index = read_json(index_path) if index_path.exists() else {}
    excl = ledger.load(root)

    probes = {}
    for p in (root / "videos").glob("*.probe.json"):
        probes[p.stem.replace(".probe", "")] = read_json(p)

    videos = []
    for rec in snapshot["videos"]:
        entry = index.get(rec["stem"])
        vid = entry["video_id"] if entry else None
        probe = probes.get(vid) if vid else None
        videos.append({
            "stem": rec["stem"], "date": rec["date"], "town": rec["town"],
            "annotated": rec["annotated"], "video_id": vid,
            "mirrored": entry is not None,
            "probed": probe is not None,
            "duration_s": probe["duration_s"] if probe else None,
            "resolution": f"{probe['width']}x{probe['height']}" if probe else None,
        })

    total_s = sum(v["duration_s"] or 0 for v in videos)
    inv = {
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "catalog_commit": snapshot["catalog_commit"],
        "videos_indexed": len(videos),
        "videos_mirrored": sum(v["mirrored"] for v in videos),
        "videos_probed": sum(v["probed"] for v in videos),
        "videos_annotated": sum(v["annotated"] for v in videos),
        "excluded_videos": len(excl["videos"]),
        "total_duration_s": round(total_s, 1),
        "rights": {"license": LICENSE_QUOTE, "media_notice": MEDIA_NOTICE_QUOTE,
                   "use_restriction": USE_RESTRICTION},
        "videos": videos,
        "exclusions": excl["videos"],
    }
    write_json(root / "reports" / "inventory.json", inv)

    lines = [
        "# Source inventory — FPV strikes catalog",
        "",
        f"Catalog commit `{snapshot['catalog_commit'][:12]}` · generated {inv['generated_utc']}",
        "",
        f"| indexed | mirrored | probed | annotated | excluded | total footage |",
        f"|---|---|---|---|---|---|",
        f"| {inv['videos_indexed']} | {inv['videos_mirrored']} | {inv['videos_probed']} "
        f"| {inv['videos_annotated']} | {inv['excluded_videos']} | {total_s/60:.0f} min |",
        "",
        "## Rights (read before any training use)",
        "",
        f"- {LICENSE_QUOTE}",
        f"- {MEDIA_NOTICE_QUOTE}",
        f"- **Bottom line:** CC0 covers only the catalog text. The video pixels carry",
        f"  no license and the content is sensitive. {USE_RESTRICTION}",
        "",
        "## Caveat: not our camera geometry",
        "",
        "Assorted 720p/1080p FPV footage supplies scale diversity and the closing",
        "dive — not the seeker's calibrated 1440×1088 / 300–500 m DRI geometry.",
        "Box sizes are reported both as-is and seeker-referenced by ds_stats.",
        "",
        "## Excluded videos",
        "",
    ]
    if excl["videos"]:
        lines += [f"- `{v['stem']}` — {v['reason_code']}: {v.get('detail','')}"
                  for v in excl["videos"]]
    else:
        lines.append("(none)")
    (root / "reports" / "inventory.md").write_text("\n".join(lines) + "\n",
                                                   encoding="utf-8")
    print(f"wrote reports/inventory.md + .json "
          f"({inv['videos_mirrored']}/{inv['videos_indexed']} mirrored)")


if __name__ == "__main__":
    main()
