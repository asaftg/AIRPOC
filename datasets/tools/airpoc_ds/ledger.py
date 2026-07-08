"""The exclusions ledger (SPEC.md §9) — every indexed video / extracted frame
that does not end up in the dataset is recorded here with a frozen reason code.
ds_validate asserts the arithmetic closes."""

from . import SPEC_VERSION, read_json, write_json

REASONS = {
    "DOWNLOAD_FAIL", "HASH_MISMATCH", "DUPLICATE_VIDEO", "PROBE_FAIL",
    "UNREADABLE_DECODE", "NO_LABELS", "LABELS_LOW_CONF_ALL", "EMPTY_GARBAGE",
    "LICENSE_HOLD", "MANUAL_EXCLUDE", "REVIEW_OVERFLOW_DROP",
}


def _path(root):
    return root / "exclusions" / "exclusions.json"


def load(root):
    p = _path(root)
    if p.exists():
        return read_json(p)
    return {"spec_version": SPEC_VERSION, "videos": [], "frames": []}


def save(root, ledger):
    write_json(_path(root), ledger)


def exclude_video(root, stem, reason_code, detail="", video_id=None):
    assert reason_code in REASONS, reason_code
    ledger = load(root)
    key = (stem, reason_code)
    if not any((v["stem"], v["reason_code"]) == key for v in ledger["videos"]):
        ledger["videos"].append({"stem": stem, "video_id": video_id,
                                 "reason_code": reason_code, "detail": detail})
        save(root, ledger)
    return ledger


def exclude_frames(root, entries):
    """Bulk frame exclusion: entries = [{source_video_id, timestamp_ms,
    extraction_mode, reason_code, detail}]. Deduplicated on (video, ms, reason)."""
    ledger = load(root)
    seen = {(f["source_video_id"], f["timestamp_ms"], f["reason_code"])
            for f in ledger["frames"]}
    for e in entries:
        assert e["reason_code"] in REASONS, e["reason_code"]
        k = (e["source_video_id"], e["timestamp_ms"], e["reason_code"])
        if k not in seen:
            ledger["frames"].append(e)
            seen.add(k)
    save(root, ledger)
    return ledger
