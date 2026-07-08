#!/usr/bin/env python3
"""Clone + parse the FPV-strikes catalog, pin the commit, write the snapshot.

The catalog repo is metadata-only (small, GitHub-reachable everywhere); the
videos live behind CloudFront and are downloaded later by ds_download.py.
The catalog is "constantly updated" — the snapshot pins the exact commit this
build read, and every later stage works from the snapshot, never the live repo.

  ds_catalog_fetch.py --root <DATASET_ROOT>            fetch + write snapshot
  ds_catalog_fetch.py --dry-run                        parse + report only
  ds_catalog_fetch.py --catalog-dir <existing-clone>   reuse a clone (offline)
  --probe-cdn N        HEAD-check the first N video URLs (honest reachability)
"""

import argparse
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import CATALOG_REPO, SPEC_VERSION, dataset_root, ensure_layout, write_json
from airpoc_ds.catalog import join


def clone_catalog(dest: Path) -> Path:
    subprocess.run(["git", "clone", "--depth", "1", CATALOG_REPO, str(dest)],
                   check=True)
    return dest


def head_commit(clone: Path) -> str:
    return subprocess.run(["git", "-C", str(clone), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()


def probe_url(url: str, timeout=15):
    req = urllib.request.Request(url, method="HEAD",
                                 headers={"User-Agent": "airpoc-datasets/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return {"url": url, "status": r.status,
                    "bytes": int(r.headers.get("Content-Length", 0))}
    except Exception as e:  # noqa: BLE001 - recorded honestly, never fatal
        return {"url": url, "status": None, "error": str(e)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--catalog-dir", help="existing catalog clone to reuse")
    ap.add_argument("--dry-run", action="store_true",
                    help="parse + report; write nothing")
    ap.add_argument("--probe-cdn", type=int, default=0, metavar="N",
                    help="HEAD-check the first N video URLs")
    args = ap.parse_args()

    tmp = None
    if args.catalog_dir:
        clone = Path(args.catalog_dir)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="fpv_catalog_")
        clone = clone_catalog(Path(tmp.name) / "catalog")
    commit = head_commit(clone)

    records, orphans = join(clone)
    annotated = sum(r["annotated"] for r in records)
    print(f"catalog commit {commit[:12]}: {len(records)} videos indexed, "
          f"{annotated} with segment annotations, "
          f"{len(orphans)} orphan annotation files (renamed/historical)")

    probes = [probe_url(r["video_url"]) for r in records[: args.probe_cdn]]
    for p in probes:
        state = f"HTTP {p['status']}" if p.get("status") else f"UNREACHABLE ({p.get('error')})"
        print(f"  cdn: {state}  {p['url']}")

    if args.dry_run:
        print(f"dry-run: would mirror {len(records)} videos; nothing written")
        return

    root = ensure_layout(dataset_root(args.root))
    snapshot = {
        "spec_version": SPEC_VERSION,
        "catalog_repo": CATALOG_REPO,
        "catalog_commit": commit,
        "fetched_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "videos": records,
        "annotation_orphans": orphans,
        "cdn_probe": probes,
    }
    out = root / "catalog" / "catalog_snapshot.json"
    write_json(out, snapshot)
    print(f"wrote {out}")
    if tmp:
        tmp.cleanup()


if __name__ == "__main__":
    main()
