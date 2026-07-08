"""Content hashing + deterministic id derivation (SPEC.md §2)."""

import hashlib
import re
from pathlib import Path

_SLUG_RE = re.compile(r"[^a-z0-9_-]+")


def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def slugify(stem: str, max_len=40) -> str:
    s = _SLUG_RE.sub("_", stem.lower()).strip("_")
    return s[:max_len].rstrip("_")


def video_id(stem: str, sha256_hex: str) -> str:
    """<slug>_<sha10>: content-addressed, stable across catalog renames."""
    return f"{slugify(stem)}_{sha256_hex[:10]}"


def stable_int_id(key: str) -> int:
    """48-bit deterministic int id from a string key (JSON-safe, never 0)."""
    v = int(hashlib.sha256(key.encode()).hexdigest()[:12], 16)
    return v or 1


def image_int_id(vid: str, ms: int, mode: str) -> int:
    return stable_int_id(f"{vid}:{ms:08d}:{mode}")


def annotation_int_id(image_id: int, bbox, category_id: int, label_source: str) -> int:
    x, y, w, h = bbox
    return stable_int_id(f"{image_id}:{x}:{y}:{w}:{h}:{category_id}:{label_source}")
