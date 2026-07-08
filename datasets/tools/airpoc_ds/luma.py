"""RGB -> 8-bit luma with frozen coefficients (SPEC.md §4).

BT.709 for HD (height >= 720), BT.601 below. TV-range input (16-235) is
expanded to full range first. The conversion actually used is recorded per
image. Luma is a proxy for the seeker's mono sensor, not a spectral match.
"""

import numpy as np

BT709 = (0.2126, 0.7152, 0.0722)
BT601 = (0.299, 0.587, 0.114)


def pick_conversion(height: int):
    """-> (name, coeffs). Frozen rule: >=720 rows is BT.709, else BT.601."""
    if height >= 720:
        return "bt709_full", BT709
    return "bt601_full", BT601


def tv_to_full(channel: np.ndarray) -> np.ndarray:
    """Expand limited range 16-235 to 0-255 (float64 out, clipped)."""
    return np.clip((channel.astype(np.float64) - 16.0) * (255.0 / 219.0), 0.0, 255.0)


def rgb_to_luma8(rgb: np.ndarray, color_range: str = "pc"):
    """HxWx3 uint8 RGB -> (HxW uint8 luma, conversion_name).

    color_range is the range of THIS rgb buffer ("tv" only if the decoder
    handed us limited-range RGB; ffmpeg's yuv->rgb24 output is full range).
    """
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError(f"expected HxWx3 RGB, got {rgb.shape}")
    name, (kr, kg, kb) = pick_conversion(rgb.shape[0])
    f = rgb.astype(np.float64)
    if color_range == "tv":
        f = np.stack([tv_to_full(f[..., i]) for i in range(3)], axis=-1)
    y = kr * f[..., 0] + kg * f[..., 1] + kb * f[..., 2]
    return np.clip(np.rint(y), 0, 255).astype(np.uint8), name
