"""Near-duplicate frame rejection: 64-bit dHash + Hamming inside a temporal
window (SPEC.md §4). Perceptual-hash based on purpose — global FPV camera
shake makes frame-diff/SSIM useless, while dHash keys on coarse structure.
Used in BASELINE mode only; terminal dedup is disabled (max_hamming=None) so
the closing sweep is kept whole (dHash is scale-tolerant and would drop a
growing target — see DEDUP_HAMMING_TERMINAL).
"""

from collections import deque

import numpy as np
from PIL import Image


def dhash64(gray: np.ndarray) -> int:
    """HxW uint8 -> 64-bit difference hash (9x8 downsample, row gradients)."""
    img = Image.fromarray(gray).resize((9, 8), Image.BILINEAR)
    p = np.asarray(img, dtype=np.int16)
    bits = (p[:, 1:] > p[:, :-1]).flatten()
    v = 0
    for b in bits:
        v = (v << 1) | int(b)
    return v


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


class TemporalDeduper:
    """Sliding-window dedup: a frame is a duplicate if any frame within
    window_s has a dHash within max_hamming.

    max_hamming=None disables dedup entirely (every frame kept) — used for the
    terminal sweep, which must never be decimated (see DEDUP_HAMMING_TERMINAL).
    """

    def __init__(self, window_s: float, max_hamming):
        self.window_ms = int(window_s * 1000)
        self.max_hamming = max_hamming
        self._recent = deque()  # (t_ms, hash)

    def is_duplicate(self, t_ms: int, gray: np.ndarray) -> bool:
        if self.max_hamming is None:
            return False
        h = dhash64(gray)
        while self._recent and t_ms - self._recent[0][0] > self.window_ms:
            self._recent.popleft()
        dup = any(hamming(h, prev) <= self.max_hamming for _, prev in self._recent)
        if not dup:
            self._recent.append((t_ms, h))
        return dup
