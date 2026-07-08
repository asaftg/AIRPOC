import numpy as np

from airpoc_ds.terminal import (fallback_terminal_window, flight_windows,
                                foreground_area, looming_fires, sample_plan,
                                terminal_windows)

SEGMENTS = [
    {"time": 0.0, "type": "banner_start"},
    {"time": 2.0, "type": "flight_start"},
    {"time": 20.0, "type": "replay_start"},
    {"time": 28.0, "type": "pause_start"},
]
DURATION = 30.0


def test_flight_window_is_marker_to_next():
    assert flight_windows(SEGMENTS, DURATION) == [(2.0, 20.0)]


def test_terminal_window_is_tail_of_flight():
    assert terminal_windows(SEGMENTS, DURATION, tail_s=6.0) == [(14.0, 20.0)]


def test_terminal_tail_clamps_to_flight_start():
    short = [{"time": 5.0, "type": "flight_start"}, {"time": 8.0, "type": "pause_start"}]
    assert terminal_windows(short, 10.0, tail_s=6.0) == [(5.0, 8.0)]


def test_sample_plan_skips_banner_and_pause():
    plan = sample_plan(SEGMENTS, DURATION, baseline_fps=2.0, terminal_fps=15.0,
                       tail_s=6.0)
    times = [p["t_ms"] / 1000.0 for p in plan]
    assert all(t >= 2.0 for t in times)              # nothing from the banner
    assert all(t < 28.0 for t in times)              # nothing from the pause
    assert any(p["from_replay"] for p in plan)       # replay region sampled+flagged
    terminal = [p for p in plan if p["mode"] == "t"]
    assert terminal and all(14.0 <= p["t_ms"] / 1000.0 < 20.0 for p in terminal)


def test_terminal_denser_than_baseline():
    plan = sample_plan(SEGMENTS, DURATION, 2.0, 15.0, 6.0)
    # 6 s terminal @15fps dominates the box-count over 12 s baseline flight @2fps
    assert sum(p["mode"] == "t" for p in plan) > sum(p["mode"] == "b" for p in plan)


def test_fallback_when_no_segments():
    plan = sample_plan([], 15.0, 2.0, 15.0, 6.0)
    assert plan and any(p["mode"] == "t" for p in plan)
    assert fallback_terminal_window(15.0, 6.0) == (9.0, 15.0)


def test_looming_fires_on_growth_not_on_shake():
    # a genuinely growing bright blob -> looming cue fires
    growing = []
    for s in range(4, 60, 3):
        img = np.full((240, 320), 60, dtype=np.uint8)
        img[120 - s // 2:120 + s // 2, 160 - s // 2:160 + s // 2] = 240
        growing.append(foreground_area(img))
    assert looming_fires(growing)

    # same blob translated around (global shake) but constant size -> no fire
    shaking = []
    rng = np.random.default_rng(0)
    for _ in range(20):
        img = np.full((240, 320), 60, dtype=np.uint8)
        cx, cy = rng.integers(40, 280), rng.integers(40, 200)
        img[cy - 10:cy + 10, cx - 10:cx + 10] = 240
        shaking.append(foreground_area(img))
    assert not looming_fires(shaking)
