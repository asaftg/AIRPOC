#!/usr/bin/env python3
"""Mirror the source MP4s from CloudFront into DATASET_ROOT/videos/.

Resumable by design (HTTP Range onto a .part file): this host is known to
drop external USB overnight, so a run must survive interruption. On
completion the file is content-addressed: sha256 -> video_id -> rename to
<video_id>.mp4, recorded in videos/index.json. A second URL whose bytes hash
identically is recorded once; failures land in the exclusions ledger as
DOWNLOAD_FAIL — counts are always used/indexed, never just used.

Only the CloudFront URLs from the snapshot are used (direct S3 is 403 by
design). The mirror is QUARANTINED: sensitive third-party media, research
use only, never redistributed.

  ds_download.py --root <DATASET_ROOT> [--limit N] [--retries K]
"""

import argparse
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from airpoc_ds import dataset_root, read_json, write_json
from airpoc_ds import ledger
from airpoc_ds.hashing import sha256_file, video_id

CHUNK = 1 << 20


def fetch(url: str, part: Path, timeout=60):
    """Stream url -> part, resuming from the current .part size."""
    have = part.stat().st_size if part.exists() else 0
    headers = {"User-Agent": "airpoc-datasets/1.0"}
    if have:
        headers["Range"] = f"bytes={have}-"
    req = urllib.request.Request(url, headers=headers)
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        if e.code == 416:  # range past EOF -> .part is already complete
            return
        raise
    mode = "ab" if have and resp.status == 206 else "wb"
    with resp, open(part, mode) as f:
        while True:
            b = resp.read(CHUNK)
            if not b:
                break
            f.write(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", help="DATASET_ROOT (or env)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N videos (testing)")
    ap.add_argument("--retries", type=int, default=3)
    args = ap.parse_args()

    root = dataset_root(args.root)
    snapshot = read_json(root / "catalog" / "catalog_snapshot.json")
    vdir = root / "videos"
    incoming = vdir / "_incoming"
    incoming.mkdir(parents=True, exist_ok=True)
    index_path = vdir / "index.json"
    index = read_json(index_path) if index_path.exists() else {}

    todo = [r for r in snapshot["videos"] if r["stem"] not in index]
    if args.limit:
        todo = todo[: args.limit]
    print(f"{len(index)} already mirrored, {len(todo)} to fetch")

    ok = fail = dup = 0
    for i, rec in enumerate(todo, 1):
        stem, url = rec["stem"], rec["video_url"]
        part = incoming / f"{stem}.mp4.part"
        print(f"[{i}/{len(todo)}] {stem}")
        err = None
        for attempt in range(args.retries):
            try:
                fetch(url, part)
                err = None
                break
            except Exception as e:  # noqa: BLE001 - logged + ledgered
                err = str(e)
                print(f"  attempt {attempt + 1} failed: {err}")
                time.sleep(5 * (attempt + 1))
        if err:
            ledger.exclude_video(root, stem, "DOWNLOAD_FAIL", detail=err)
            fail += 1
            continue

        digest = sha256_file(part)
        vid = video_id(stem, digest)
        clash = next((s for s, v in index.items() if v["sha256"] == digest), None)
        if clash:
            ledger.exclude_video(root, stem, "DUPLICATE_VIDEO",
                                 detail=f"same bytes as {clash}", video_id=vid)
            part.unlink()
            dup += 1
        else:
            dest = vdir / f"{vid}.mp4"
            part.replace(dest)
            index[stem] = {"video_id": vid, "sha256": digest, "url": url,
                           "bytes": dest.stat().st_size}
            write_json(index_path, index)
            ok += 1

    print(f"done: {ok} mirrored, {dup} duplicate, {fail} failed "
          f"({len(index)}/{len(snapshot['videos'])} total mirrored)")
    if fail:
        sys.exit(1)


if __name__ == "__main__":
    main()
