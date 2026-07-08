import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ds_split import assign_stratum, bucket


def test_split_is_deterministic():
    vids = [f"v_{i:010x}" for i in range(50)]
    a = assign_stratum(vids, 42)
    b = assign_stratum(vids, 42)
    assert a == b


def test_no_video_in_two_splits():
    vids = [f"v_{i:010x}" for i in range(50)]
    assign = assign_stratum(vids, 7)
    assert set(assign.values()) <= {"train", "val", "test"}
    # each video appears exactly once (dict keys are unique by construction)
    assert len(assign) == len(vids)


def test_all_three_splits_populated_when_enough_videos():
    vids = [f"v_{i:010x}" for i in range(30)]
    assign = assign_stratum(vids, 1)
    assert set(assign.values()) == {"train", "val", "test"}


def test_tiny_stratum_no_fixup_crash():
    # 2 videos: fix-up must not run (needs >=3), and must not throw
    assign = assign_stratum(["v_0000000001", "v_0000000002"], 3)
    assert len(assign) == 2


def test_ratios_roughly_hold_on_large_set():
    vids = [f"v_{i:010x}" for i in range(1000)]
    assign = assign_stratum(vids, 99)
    frac_train = sum(v == "train" for v in assign.values()) / len(vids)
    assert 0.72 < frac_train < 0.88  # ~0.8 with hash noise


def test_bucket_in_unit_interval():
    for i in range(100):
        u = bucket(5, f"v_{i:010x}")
        assert 0.0 <= u < 1.0
