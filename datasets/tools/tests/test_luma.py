import numpy as np

from airpoc_ds.luma import BT601, BT709, pick_conversion, rgb_to_luma8, tv_to_full


def test_pick_conversion_hd_vs_sd():
    assert pick_conversion(1080) == ("bt709_full", BT709)
    assert pick_conversion(720) == ("bt709_full", BT709)
    assert pick_conversion(480) == ("bt601_full", BT601)


def test_pure_gray_is_identity():
    rgb = np.full((480, 640, 3), 123, dtype=np.uint8)
    y, name = rgb_to_luma8(rgb)
    assert name == "bt601_full"
    assert np.all(y == 123)  # coeffs sum to 1 -> gray maps to itself


def test_pure_channels_match_coeffs():
    for h, coeffs, expect_name in ((480, BT601, "bt601_full"),
                                   (1080, BT709, "bt709_full")):
        for ch, k in enumerate(coeffs):
            rgb = np.zeros((h, 8, 3), dtype=np.uint8)
            rgb[..., ch] = 255
            y, name = rgb_to_luma8(rgb)
            assert name == expect_name
            assert abs(int(y[0, 0]) - round(255 * k)) <= 1


def test_tv_to_full_endpoints():
    ch = np.array([[16, 235]], dtype=np.uint8)
    out = tv_to_full(ch)
    assert abs(out[0, 0] - 0.0) < 1e-6
    assert abs(out[0, 1] - 255.0) < 1e-6


def test_tv_range_expands():
    rgb = np.full((480, 4, 3), 16, dtype=np.uint8)  # TV black
    y, _ = rgb_to_luma8(rgb, color_range="tv")
    assert np.all(y == 0)
