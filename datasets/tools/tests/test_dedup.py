import numpy as np

from airpoc_ds import DEDUP_HAMMING_BASELINE, DEDUP_HAMMING_TERMINAL
from airpoc_ds.dedup import TemporalDeduper, dhash64, hamming


def frame(rect):
    img = np.full((240, 320), 60, dtype=np.uint8)
    x, y, w, h = rect
    img[y:y + h, x:x + w] = 220
    return img


def test_identical_is_duplicate():
    d = TemporalDeduper(2.0, DEDUP_HAMMING_BASELINE)
    a = frame((100, 100, 40, 30))
    assert d.is_duplicate(0, a) is False       # first is always kept
    assert d.is_duplicate(200, a.copy()) is True


def test_terminal_dedup_disabled_keeps_everything():
    # DEDUP_HAMMING_TERMINAL is None: the closing sweep is kept WHOLE. Even a
    # byte-identical frozen repeat is kept, because dHash cannot safely tell a
    # growing target from a repeat and decimating the sweep is the cardinal sin.
    assert DEDUP_HAMMING_TERMINAL is None
    d = TemporalDeduper(2.0, DEDUP_HAMMING_TERMINAL)
    a = frame((150, 110, 20, 20))
    assert d.is_duplicate(0, a) is False
    assert d.is_duplicate(66, a.copy()) is False          # identical still kept
    for i, s in enumerate((12, 20, 28, 36)):              # growing dive: all kept
        r = (150 - s // 2, 110 - s // 2, s, s)
        assert d.is_duplicate(100 + i * 66, frame(r)) is False


def test_baseline_still_dedups_static_repeat():
    d = TemporalDeduper(2.0, DEDUP_HAMMING_BASELINE)
    a = frame((100, 100, 40, 30))
    assert d.is_duplicate(0, a) is False
    assert d.is_duplicate(200, a.copy()) is True


def test_window_expiry_forgets_old():
    d = TemporalDeduper(2.0, DEDUP_HAMMING_BASELINE)
    a = frame((100, 100, 40, 30))
    assert d.is_duplicate(0, a) is False
    # same content but far outside the 2 s window -> no longer compared -> kept
    assert d.is_duplicate(5000, a.copy()) is False


def test_hamming_and_hash_stable():
    a = frame((100, 100, 40, 30))
    assert hamming(dhash64(a), dhash64(a.copy())) == 0
