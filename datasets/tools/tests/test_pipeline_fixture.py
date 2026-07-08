"""End-to-end proof of the non-GPU spine on the synthetic fixture:
make_fixture -> merge -> (rescan cycle) -> split -> pack -> stats -> manifest
-> validate. No CDN, GPU, or ffmpeg touched.
"""

import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]


def run(script, root, *args):
    cmd = [sys.executable, str(TOOLS / script), "--root", str(root), *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, f"{script} failed:\n{r.stdout}\n{r.stderr}"
    return r.stdout


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    root = tmp_path_factory.mktemp("fixture_ds")
    run("ds_make_fixture.py", root)
    # first merge: some terminal frames request a rescan (0 detections)
    run("ds_merge.py", root)
    # the fixture already marks those frames rescanned=True, so a second merge
    # resolves them without a GPU rescan step
    run("ds_merge.py", root)
    run("ds_split.py", root)
    run("ds_pack.py", root, "--pending", "drop")
    run("ds_stats.py", root)
    run("ds_manifest.py", root, "--version", "0.0.1-test")
    run("ds_validate.py", root)
    return root


def read(root, *parts):
    import json
    return json.loads((root.joinpath(*parts)).read_text())


def test_validate_passes(built):
    # ds_validate already ran in the fixture; re-run to assert exit 0 explicitly
    run("ds_validate.py", built)


def test_manifest_arithmetic(built):
    m = read(built, "manifest.json")
    assert m["source"]["videos_indexed"] - m["source"]["videos_used"] \
        == m["exclusions_count"]
    assert m["spec_version"] == "layout-1"
    assert m["counts"]["annotations"] > 0


def test_splits_disjoint(built):
    vids = set()
    for split in ("train", "val", "test"):
        coco = read(built, "annotations", f"instances_{split}.json")
        s = {i["source_video_id"] for i in coco["images"]}
        assert not (s & vids), "a video leaked across splits"
        vids |= s


def test_both_classes_present(built):
    m = read(built, "manifest.json")
    assert m["counts"]["by_class"]["vehicle"] > 0
    assert m["counts"]["by_class"]["human"] > 0


def test_review_queue_capped(built):
    q = read(built, "review", "review_queue.json")
    assert q["queued"] <= q["budget"]


def test_auto_accept_and_drops_recorded(built):
    m = read(built, "manifest.json")
    rev = m["counts"]["review"]
    assert rev["auto_accepted"] > 0
    excl = read(built, "exclusions", "exclusions.json")
    reasons = {f["reason_code"] for f in excl["frames"]}
    assert "EMPTY_GARBAGE" in reasons  # the garbage frames were dropped, ledgered


def test_terminal_frames_kept(built):
    m = read(built, "manifest.json")
    assert m["counts"]["by_mode"]["terminal"] > 0
