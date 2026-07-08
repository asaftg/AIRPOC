"""Terminal-approach windows + the frame sampling plan (SPEC.md §4).

Primary path is EXACT: the catalog's human-marked segments. A flight segment
runs from a `flight_start` to the next marker of any type (or end-of-video);
its last `tail_s` seconds are the terminal window (the impact is the segment
end). Fallback for unannotated videos is an interim looming heuristic,
stamped as such.
"""

import numpy as np

FLIGHT = "flight_start"
REPLAY = "replay_start"
SKIP_TYPES = {"banner_start", "pause_start"}  # graphics / freeze — never sampled


def _regions(segments, duration_s):
    """Segment markers -> [(start, end, type)] covering marker-to-next-marker."""
    marks = sorted((s["time"], s["type"]) for s in segments)
    out = []
    for i, (t, typ) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else duration_s
        if end > t:
            out.append((t, min(end, duration_s), typ))
    return out


def flight_windows(segments, duration_s):
    return [(s, e) for s, e, t in _regions(segments, duration_s) if t == FLIGHT]


def terminal_windows(segments, duration_s, tail_s):
    """Last tail_s of every flight segment — exact, from human annotations."""
    return [(max(s, e - tail_s), e) for s, e in flight_windows(segments, duration_s)]


def fallback_terminal_window(duration_s, tail_s):
    """No annotations: anchor on end-of-stream. Interim (looming_heuristic)."""
    return (max(0.0, duration_s - tail_s), duration_s)


def foreground_area(gray: np.ndarray, k: float = 2.0) -> float:
    """Fraction of pixels brighter than mean + k*sigma. Translation-invariant,
    so global FPV shake does not move it — only a genuinely growing target does."""
    f = gray.astype(np.float64)
    thr = f.mean() + k * f.std()
    return float((f > thr).mean())


def looming_fires(areas, growth: float = 2.0, smooth: int = 5) -> bool:
    """Gate for the fallback window: fires only if the smoothed foreground
    area grows by >= `growth`x from the start to the end of the series."""
    a = np.asarray(areas, dtype=np.float64)
    if len(a) < 2 * smooth:
        return False
    kernel = np.ones(smooth) / smooth
    s = np.convolve(a, kernel, mode="valid")
    head = s[:smooth].mean()
    tail = s[-smooth:].mean()
    if head <= 0:
        return tail > 0
    return (tail / head) >= growth


def sample_plan(segments, duration_s, baseline_fps, terminal_fps, tail_s):
    """-> sorted [{t_ms, mode ('b'|'t'), from_replay}].

    With segments: flight + replay regions are sampled (baseline rate), the
    tail of each flight segment densely (terminal rate); banner/pause/other
    regions are skipped. Without segments: whole video at baseline rate plus
    a fallback terminal tail (caller stamps terminal_source accordingly).
    """
    plan = {}  # t_ms -> (mode, from_replay); terminal wins collisions

    def add(start, end, fps, mode, from_replay):
        step = 1.0 / fps
        t = start
        while t < end - 1e-9:
            t_ms = int(round(t * 1000))
            if mode == "t" or t_ms not in plan:
                plan[t_ms] = (mode, from_replay)
            t += step

    if segments:
        regions = _regions(segments, duration_s)
        for s, e, typ in regions:
            if typ == FLIGHT:
                add(s, e, baseline_fps, "b", False)
            elif typ == REPLAY:
                add(s, e, baseline_fps, "b", True)
        for s, e in terminal_windows(segments, duration_s, tail_s):
            add(s, e, terminal_fps, "t", False)
    else:
        add(0.0, duration_s, baseline_fps, "b", False)
        s, e = fallback_terminal_window(duration_s, tail_s)
        add(s, e, terminal_fps, "t", False)

    return [{"t_ms": t, "mode": m, "from_replay": r}
            for t, (m, r) in sorted(plan.items())]
