"""Parse the FPV-strikes catalog clone: geo CSV (the URL index) + the
human-marked annotation JSONs, joined by video stem (SPEC.md §3-§4).

The catalog is a moving target ("constantly updated") — every build pins the
commit it read, and counts are whatever that commit says.
"""

import csv
import json
from pathlib import Path
from urllib.parse import unquote, urlparse

GEO_CSV = Path("geo") / "fpv_drone_map_records.csv"
ANNOTATIONS_DIR = Path("annotations")
ANNOTATION_SUFFIX = "_annotations.json"


def stem_from_url(url: str) -> str:
    return unquote(Path(urlparse(url).path).stem)


def parse_geo_csv(catalog_dir: Path):
    """-> [{stem, date, description, town, lat, lon, status, video_url, thumbnail_url}]"""
    rows = []
    with open(catalog_dir / GEO_CSV, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            url = (row.get("video_url") or "").strip()
            if not url:
                continue
            rows.append({
                "stem": stem_from_url(url),
                "date": row.get("date", ""),
                "description": row.get("description", ""),
                "town": row.get("town", ""),
                "lat": row.get("lat", ""),
                "lon": row.get("lon", ""),
                "status": row.get("status", ""),
                "video_url": url,
                "thumbnail_url": (row.get("thumbnail_url") or "").strip(),
            })
    return rows


def load_annotations(catalog_dir: Path):
    """-> {stem: annotation_json} for every annotations/<stem>_annotations.json"""
    out = {}
    ann_dir = catalog_dir / ANNOTATIONS_DIR
    if not ann_dir.is_dir():
        return out
    for p in sorted(ann_dir.glob(f"*{ANNOTATION_SUFFIX}")):
        stem = p.name[: -len(ANNOTATION_SUFFIX)]
        try:
            out[stem] = json.loads(p.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            out[stem] = {"_parse_error": str(e)}
    return out


def join(catalog_dir: Path):
    """One record per geo-CSV video, with its annotation attached when a
    matching stem exists. -> [{...geo fields, segments, exclusion_masks,
    annotated}] plus the list of annotation stems that matched no video
    (renamed/historical entries — reported, not used)."""
    geo = parse_geo_csv(catalog_dir)
    anns = load_annotations(catalog_dir)
    matched = set()
    records = []
    for rec in geo:
        ann = anns.get(rec["stem"])
        if ann is not None and "_parse_error" not in ann:
            matched.add(rec["stem"])
            rec = dict(rec, annotated=True,
                       segments=ann.get("segments", []),
                       exclusion_masks=ann.get("exclusion_masks", []))
        else:
            rec = dict(rec, annotated=False, segments=[], exclusion_masks=[])
        records.append(rec)
    orphans = sorted(set(anns) - matched)
    return records, orphans
