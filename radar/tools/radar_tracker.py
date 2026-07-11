#!/usr/bin/env python3
"""Approach B: track-before-detect multi-target radar tracker.

Two measurement channels feed ONE tracker:
  * MOVING channel: points with |doppler| >= vmin, inside the ground-target
    elevation band. (Doppler is only a motion FLAG - speed comes from history.)
  * FRESH-STATIC channel: high-SNR near-zero-doppler returns at (r,az) cells
    that were historically EMPTY in a slowly-learned occupancy map. These may
    only SUSTAIN tracks that already proved themselves with Doppler (a car that
    drives in and stops keeps its track while parked); they never birth tracks.

Tracking:
  * State (r, az, vr, va); velocity ONLY from a least-squares fit of measured
    position history (range-rate / angle-rate), never from Doppler.
  * Association claims ALL points inside an adaptive gate around the
    prediction (grows with speed and misses) -> a big near target stays ONE track.
  * Confirmation: fast M-of-N for Doppler-backed tracks; slower window plus a
    low jitter/consistency requirement for static-born tracks. No minimum
    displacement requirement anywhere (that's what blinded the baselines to 247).
  * Confirmed tracks coast briefly; emission stops before death (false guard).
"""
import math
from collections import deque
import numpy as np

DEFAULTS = dict(
    # ---- point selection (shared) ----
    # elevation gate: symmetric half-angle, mirrors the C elmax knob (the old
    # hard -9..+2.5 level-mount window was removed from the C port 2026-07-10;
    # pass el_lo/el_hi explicitly to reproduce legacy behaviour)
    el_lo=-20.0, el_hi=20.0,
    az_keep=90.0,            # mirrors the C fov knob default (input az gate)
    r_min=3.0,
    # moving channel
    vmin=0.7,
    snr_mv=16.0,
    # fresh-static channel
    snr_st=19.0,
    occ_free=0.35,       # cell (3x3 max) occupancy below this = historically empty
    warmup_s=8.0,        # build map only, no fresh-static output
    learn_fast=0.10,     # occupancy learn rate during warmup
    learn=0.0003,        # occupancy learn rate after warmup (a new static needs ~17s+ to
                         # become background -> idling car stays "fresh", ghosts die out)
    decay=0.00005,
    st_min_pts=2,        # fresh-static cluster size to be a measurement
    st_sustain_mv=1.2,   # static points may only sustain tracks whose peak moving
                         # support reached this (no static-born ghost tracks)
    st_emit_cap=100000,  # effectively uncapped: parked real movers keep emitting
    st_mv_lo=0.35,       # mv_ewma below this counts as "no Doppler support"
    # ---- gating ----
    gate_r=6.0,
    gate_cross=4.0,
    az_gate_min=1.0,
    az_gate_max=8.0,
    speed_gate_s=0.45,
    miss_grow=0.30,
    miss_grow_cap=2.0,
    conf_grow_cap=1.5,   # confirmed tracks: tighter cap on miss-driven gate
                         # growth (an established track that big-grows its gate
                         # is exactly how a ghost latches onto fresh noise)
    # ---- confirmation ----
    conf_M=3, conf_N=4,      # fast path (Doppler-backed)
    mv_rate_min=0.6,         # mean moving pts/frame needed for the fast path
    jit_max=2.6,
    st_conf_M=8, st_conf_N=12,  # slow path (static-born)
    st_jit_max=1.8,
    jit_alpha=0.35,
    # ---- lifecycle ----
    tent_max_miss=3,
    conf_max_miss=10,
    emit_max_miss=3,
    # parked persistence: a track that has displaced, then stopped, then lost
    # all returns (car parked, engine off -> indistinguishable from background)
    # holds its frozen position instead of dying
    park_spd=0.6,        # m/s: below this a starving track counts as parked
    park_disp=1.8,       # m: recent (4 s) displacement below this = stationary
    park_win=4.0,
    park_travel=4.0,     # m: net travel since birth required to earn park-coast
    park_miss=390,       # frames (~15 s) a parked track may hold + emit
    # ---- velocity from history ----
    vel_win=0.9,
    vel_min_span=0.22,
    speed_max=30.0,
    alpha=0.55,          # measurement blend into state (range)
    alpha_az=0.7,        # az blends faster (az error is scored at a hard 4 deg)
    # ---- elevation (vertical) output ----
    el_off=3.1,          # radar boresight sits ~3.1 deg BELOW EO centre (measured
                         # vs EO bbox centres across all movers). add to radar el.
    el_alpha=0.10,       # heavy per-track EMA: raw el from the 2-row array is
                         # +-2-4 deg noisy while a target's true vertical spread is
                         # <1 deg, so smooth hard or the box jitters ~150px.
    # ---- seeding ----
    seed_min_pts=2,      # cluster size beyond seed_far_r (sparse far targets)
    seed_near_pts=3,     # cluster size below seed_far_r (near real targets are
                         # point-rich; near 2-pt clusters are mostly noise)
    seed_far_r=0.0,
    link_r=5.0,
    link_cross=3.5,
    seed_guard_r=5.0,
    seed_guard_az=2.5,
    # ---- dedup ----
    merge_r=12.0, merge_cross=3.0, merge_dv=1.2,
    # emission-level dedup: a weaker confirmed track co-located in SPACE (cross
    # AND range) with a stronger one is a fragment/sidelobe -> keep alive but do
    # not emit a 2nd circle. gated on range too so distinct cars at different
    # ranges stay separate; identity untouched -> no sensitivity loss, no swaps.
    dedup_cross=4.5, dedup_r=12.0,
    # ---- emission band (EO-fusable region) ----
    az_off=1.5,          # radar boresight vs EO center (deg)
    out_az=90.0,         # no artificial az clamp; az limited by az_keep (radar AoA FOV)
    out_r_max=500.0,     # full radar range. (170/400 were validation clamps tied to
                         # EO ground-truth coverage; removed -> emit everything the radar sees.)
    # a track earns emission by MOVING: current speed from position history,
    # or accumulated net displacement. (A confirmed track that never moves is
    # scenery with Doppler leakage; once it has displaced it may keep emitting
    # while parked, e.g. a car that drives in and stops.)
    emit_spd=1.2,        # m/s radial (range-rate) speed that counts as moving
    emit_disp=2.0,       # m radial displacement over the window that counts as moving
    disp_win=5.0,        # s  window for the motion test
    # A target "moves" if its RANGE changes (clean) OR it crosses by more than the
    # angle-noise floor at its distance (so far azimuth jitter isn't read as motion:
    # a static return at 400m jittering +/-1 deg = ~7m cross, which must clear
    # radians(ang_floor)*range to count). Non-latching: re-checked every frame.
    ang_floor_deg=2.5,   # deg  cross displacement must beat this angular amount
    ang_rate_min=3.0,    # deg/s real angular rate (from the velocity fit) = a crosser
    move_confirm=8,      # frames of real motion before a track may park-hold
    park_s=15.0,         # s  a genuinely-moved track may hold after it stops
    phantom_leash_s=1.3, # s  grace for a real mover's velocity to warm up; a track
                         #    that shows no genuine motion by then is a phantom -> cull
    # ---- post-confirmation consistency guard ----
    # A confirmed track must stay PHYSICALLY COHERENT. All tests are RANGE-AWARE:
    # range measurements are clean at any distance, azimuth noise scales as
    # radians(err)*range, so cross-domain evidence is only judged above the
    # angle-noise floor at the track's range. Scored over a sliding window of
    # MEASURED positions only (hits, never coasted predictions), time-binned
    # into waypoints so per-frame noise cannot masquerade as path length.
    guard=True,
    guard_win=2.5,       # s   sliding window over measured history
    guard_nseg=5,        # time bins for waypoint averaging (noise suppression)
    guard_min_n=8,       # measured points in window needed to judge
    coh_min=0.40,        # net/path below this = random walk, not a target
    r_path_min=3.0,      # m   range path below this: too little radial evidence
                         #     to judge radial coherence (tangential/slow exempt)
    r_path_rk=0.025,     # m/m range-growth of that floor: point density/multipath
                         #     thickens the noise path at range (a standing target
                         #     at 270 m walks ~4-6 m of pure-noise range path per
                         #     window under the open elevation diet)
    spd_max=36.0,        # m/s range path speed above this is unphysical
                         #     (SPEED_MAX 30 + fitting slack)
    tele_spd=45.0,       # m/s implied RANGE step speed = re-latch teleport
    tele_dm=3.0,         # m   min range step to count (body-extent jitter exempt)
    tele_rk=0.02,        # m/m range-growth of that step floor: at 275 m the
                         #     multipath mode-flip on a standing human moves the
                         #     blended position ~4-5 m/frame, which is measurement
                         #     bimodality, not a track re-latch. A true re-latch
                         #     jumps by the gate (>=6-12 m) and still trips this.
    tele_dt_max=0.5,     # s   only judge steps without a long gap
    tele_k=3,            #     teleports in window to flag
    jit_base=3.5,        # m   innovation EWMA allowance at r=0 ...
    jit_rk=0.02,         # m/m ... growing with range (~1.15 deg of az noise)
    cross_wander_k=2.0,  #     cross path must exceed k * angle-noise floor
                         #     (radians(ang_floor_deg)*r) before cross coherence
                         #     is judged (catches constant-range az wanderers)
    guard_kill=10,       # bad-frame counter (hysteresis) -> kill, not park
    guard_emit=12,       # consecutive judged-good frames before a confirmed
                         #     track may EMIT (positive coherence evidence);
                         #     latched, unlatched when counter shows trouble
    guard_emit_re=3,     # re-latch streak for a track that ALREADY earned the
                         #     emission latch once: a proven target that hits a
                         #     noisy patch (unlatch) must not re-pay the full
                         #     latch; ghosts never earn the latch, so this is
                         #     closed to them
    guard_unlatch=5,     # counter level that unlatches emission (ghosts never
                         #     latch at all, so this only trades real-target
                         #     flicker; 5 keeps a briefly-noisy real box up)
    relatch_dr=15.0,     # m   a track that EARNED the latch may re-earn it
                         #     without net progress while it stays near the
                         #     range where it last held it (a walk-then-STAND
                         #     far target keeps its box; a never-latched
                         #     wanderer gains nothing by standing)
    faint_r=200.0,       # m   beyond this, a track that has held a FULL
                         #     coherent streak may emit below the brightness
                         #     bar (a small drone at 250 m lives in the
                         #     16-17 dB band; the bar cannot separate it from
                         #     floor noise there, so coherence must)
    dop_gate=4.0,        # m/s faint-far relief also demands DOPPLER SELF-
                         #     CONSISTENCY: a real mover's claimed points carry
                         #     doppler matching its fitted range-rate (measured
                         #     p50 err 0.9-3.3 on real tracks); a spur-comb
                         #     streak's apparent motion comes from re-latching,
                         #     its point doppler is soup (p50 err 16-19).
                         #     Caveat: a fast radial target beyond the doppler
                         #     fold would fail this; acceptable for the faint-
                         #     far case (slow/small targets), documented.
    dop_alpha=0.2,       #     EWMA rate for the consistency error
    far_margin_db=2.0,   # dB  at range the plain bar sits at the noise floor,
                         #     so passing it outright needs this margin (real
                         #     far tracks measure >= +2.6 dB over the bar; spur
                         #     streaks measure +1.5); ramps in linearly over
                         #     far_margin_r0..faint_r (no cliff a streak can
                         #     duck under); below the margin the faint path
                         #     (coherence + doppler self-consistency) is the
                         #     only way to emit
    far_margin_r0=150.0, # m   margin ramp start
    pos_net=2.0,         # m   net window displacement (range, or cross above the
                         #     angle-noise floor) that counts as POSITIVE progress
                         #     evidence for the emission latch. "Not incoherent"
                         #     is not enough: a noise cluster that sits still is
                         #     unjudgeable, not a target.
    # brightness evidence: a REAL target near the radar is far brighter than the
    # CFAR floor (R^4); noise is floor-level at every range. To emit at range r
    # a track's lifetime PEAK claimed SNR must clear req(r) = clamp(hi -
    # slope*r, lo, hi) for its CURRENT range � not a latch: a far-born track
    # (floor noise passes the far requirement trivially) that wanders close
    # faces the close-range bar it can never meet, while a real approacher
    # brightens much faster (R^4) than the bar rises.
    # Measured: floor-noise ghosts peak 18-21 dB at 15-120 m; real tracks show
    # 21+ dB even at 250 m and 25-55 dB when close.
    snr_evid_hi=24.5,    # dB  requirement at r=0
    snr_evid_lo=17.5,    # dB  requirement floor (far; barely above the 16 dB chip floor)
    snr_evid_slope=0.025,  # dB/m
    # near-field flood: when something moves right next to the radar, sidelobes
    # light up the whole hemisphere (points at every az/el, r < ~40 m) and angle
    # information is physically gone. While flooded, close tracks cannot EARN
    # emission evidence (already-proven tracks are untouched).
    flood_r=40.0,        # m   the flood lives below this range
    flood_n=25,          # moving points below flood_r ...
    flood_az=80.0,       # deg ... spread over this much azimuth (p90-p10) = flood
    flood_hold=2.0,      # s   evidence freeze persists after the last flood frame
    flood_margin=10.0,   # m   freeze applies to tracks below flood_r + margin
    # emission-evidence handoff: when an EMITTING confirmed track dies of
    # DROPOUT (misses; never of a guard kill), a new track confirmed at the
    # same spot shortly after is the same physical target re-acquired -> it
    # inherits the earned emission latch instead of re-paying it. This is what
    # keeps a fragmenting real mover from losing a latch-window per fragment.
    inherit_s=2.0,       # s   graveyard memory
    inherit_r=10.0,      # m   position gate (range)
    inherit_cross=6.0,   # m   position gate (cross)
)

# occupancy grid geometry
GR_R0, GR_R1, GR_DR = 0.0, 400.0, 2.0
GR_A0, GR_A1, GR_DA = -32.0, 32.0, 1.0
NR = int((GR_R1 - GR_R0) / GR_DR)
NA = int((GR_A1 - GR_A0) / GR_DA)


class Track:
    __slots__ = ("tid", "r", "az", "el", "vr", "va", "hist", "hits", "misses",
                 "confirmed", "age", "jit", "mv_ewma", "max_mv", "st_frames",
                 "disp_flag", "r0", "az0", "born_static",
                 "moved_frames", "last_moved_t", "moving",
                 "guard_bad", "guard_pass", "ok_streak", "coh_bad",
                 "snr_peak", "ever_passed", "pass_r", "jit_ctr", "deep_pass",
                 "dop_err")

    def __init__(self, tid, t, r, az, el, n_win, born_static):
        self.tid = tid
        self.r = r
        self.az = az
        self.el = el
        self.vr = 0.0
        self.va = 0.0
        self.hist = deque(maxlen=160)
        self.hist.append((t, r, az))
        self.hits = deque(maxlen=n_win)
        self.hits.append(1)
        self.misses = 0
        self.confirmed = False
        self.age = 1
        self.jit = 0.0
        self.mv_ewma = 0.0
        self.max_mv = 0.0
        self.st_frames = 0
        self.disp_flag = False
        self.r0 = r
        self.az0 = az
        self.born_static = born_static
        self.moved_frames = 0
        self.last_moved_t = t
        self.moving = False
        self.guard_bad = 0
        self.guard_pass = False
        self.ok_streak = 0
        self.coh_bad = False
        self.snr_peak = 0.0
        self.ever_passed = False
        self.pass_r = r
        self.jit_ctr = 0
        self.deep_pass = False
        self.dop_err = 0.0

    def travel(self):
        return math.hypot(self.r - self.r0,
                          math.radians(self.az - self.az0) * self.r)

    def displaced(self, tnow, spd_min, disp_min, win):
        """Sticky: has this track ever shown coherent motion? Speed (from
        position history) short-circuits; otherwise first-third vs last-third
        mean position over the trailing window must differ by disp_min."""
        if self.disp_flag:
            return True
        if math.hypot(self.vr, math.radians(self.va) * self.r) >= spd_min:
            self.disp_flag = True
            return True
        if self.recent_disp(tnow, win) >= disp_min:
            self.disp_flag = True
        return self.disp_flag

    def recent_disp(self, tnow, win):
        """Coherent displacement (m) over the trailing window: first-third vs
        last-third mean position of MEASURED history."""
        h = [s for s in self.hist if tnow - s[0] <= win]
        n = len(h)
        if n < 9:
            return 0.0
        k = n // 3
        r1 = sum(s[1] for s in h[:k]) / k
        r2 = sum(s[1] for s in h[-k:]) / k
        a1 = sum(s[2] for s in h[:k]) / k
        a2 = sum(s[2] for s in h[-k:]) / k
        cross = math.radians(a2 - a1) * 0.5 * (r1 + r2)
        return math.hypot(r2 - r1, cross)

    def recent_motion(self, tnow, win):
        """(radial, cross) coherent displacement (m) over the window: first-third
        vs last-third mean position. Radial (range) is clean; cross (azimuth) is
        noisy and grows with range, so callers threshold it against range."""
        h = [s for s in self.hist if tnow - s[0] <= win]
        n = len(h)
        if n < 9:
            return 0.0, 0.0
        k = n // 3
        r1 = sum(s[1] for s in h[:k]) / k
        r2 = sum(s[1] for s in h[-k:]) / k
        a1 = sum(s[2] for s in h[:k]) / k
        a2 = sum(s[2] for s in h[-k:]) / k
        return abs(r2 - r1), abs(math.radians(a2 - a1)) * 0.5 * (r1 + r2)

    def guard_metrics(self, tnow, win, nseg, tele_spd, tele_dm, tele_dt_max):
        """Physical-coherence evidence over the trailing window of MEASURED
        positions: (n, r_path, r_net, r_span, c_path, c_net, tele).

        Path/net are computed per-DOMAIN over TIME-BINNED waypoints (the window
        is split into nseg bins; each occupied bin contributes its mean
        position, which suppresses per-frame noise ~sqrt(frames/bin)):
          * range domain  — clean at any distance; a ghost re-latching across
            clutter racks up range path that goes nowhere (net << path), a real
            radial mover is ballistic (net ~= path).
          * cross domain  — az noise scales with range, so callers must gate
            on the angle-noise floor before judging cross coherence.
        Teleports count raw consecutive RANGE steps that are both large
        (> tele_dm, so body-extent jitter is exempt) and imply impossible
        radial speed (> tele_spd)."""
        h = [s for s in self.hist if tnow - s[0] <= win]
        n = len(h)
        if n < 2:
            return n, 0.0, 0.0, 0.0, 0.0, 0.0, 0
        tele = 0
        for i in range(1, n):
            sdt = h[i][0] - h[i-1][0]
            dr = abs(h[i][1] - h[i-1][1])
            if 0.0 < sdt <= tele_dt_max and dr > tele_dm and dr / sdt > tele_spd:
                tele += 1
        # time-binned waypoints
        tlo = h[0][0]
        seg_dt = max((h[-1][0] - tlo) / nseg, 1e-9)
        wp = []                      # (t_mean, r_mean, az_mean) per occupied bin
        b = [0.0, 0.0, 0.0, 0]       # accum t, r, az, count
        cur = 0
        for (ts, r, az) in h:
            k = min(int((ts - tlo) / seg_dt), nseg - 1)
            if k != cur and b[3]:
                wp.append((b[0]/b[3], b[1]/b[3], b[2]/b[3]))
                b = [0.0, 0.0, 0.0, 0]
                cur = k
            b[0] += ts; b[1] += r; b[2] += az; b[3] += 1
        if b[3]:
            wp.append((b[0]/b[3], b[1]/b[3], b[2]/b[3]))
        if len(wp) < 3:
            return n, 0.0, 0.0, 0.0, 0.0, 0.0, tele
        r_path = 0.0
        c_path = 0.0
        for i in range(1, len(wp)):
            r_path += abs(wp[i][1] - wp[i-1][1])
            c_path += abs(math.radians(wp[i][2] - wp[i-1][2])) \
                      * 0.5 * (wp[i][1] + wp[i-1][1])
        r_net = abs(wp[-1][1] - wp[0][1])
        c_net = abs(math.radians(wp[-1][2] - wp[0][2])) \
                * 0.5 * (wp[0][1] + wp[-1][1])
        r_span = wp[-1][0] - wp[0][0]
        return n, r_path, r_net, r_span, c_path, c_net, tele


def _cluster(pts, link_r, link_cross):
    n = len(pts)
    used = [False] * n
    clusters = []
    for i in range(n):
        if used[i]:
            continue
        stack = [i]
        used[i] = True
        members = []
        while stack:
            j = stack.pop()
            members.append(j)
            rj, aj = pts[j][0], pts[j][1]
            for k in range(n):
                if used[k]:
                    continue
                rk, ak = pts[k][0], pts[k][1]
                if abs(rk - rj) < link_r:
                    cross = abs(math.radians(ak - aj)) * 0.5 * (rk + rj)
                    if cross < link_cross:
                        used[k] = True
                        stack.append(k)
        clusters.append(members)
    return clusters


def tracker(frames, **kw):
    P = dict(DEFAULTS)
    P.update(kw)
    n_win = max(P["conf_N"], P["st_conf_N"])
    occ = np.zeros((NR, NA), dtype=np.float32)
    out = []
    tracks = []
    next_id = 1
    _D = [0, 0, 0, 0]   # [max_tracks, max_confirmed, max_moving, seeded]
    t0 = frames[0]["t"]
    tprev = t0
    flood_until = t0 - 1.0
    grave = []          # (t_death, r, az, evid_ok) of emitting confirmed dropouts

    def refit_vel(tk, tnow):
        h = [s for s in tk.hist if tnow - s[0] <= P["vel_win"]]
        if len(h) < 3:
            return
        ts = np.array([s[0] for s in h])
        if ts[-1] - ts[0] < P["vel_min_span"]:
            return
        rs_ = np.array([s[1] for s in h])
        azs_ = np.array([s[2] for s in h])
        tm = ts - ts.mean()
        den = float((tm * tm).sum())
        tk.vr = float((tm * (rs_ - rs_.mean())).sum() / den)
        tk.va = float((tm * (azs_ - azs_.mean())).sum() / den)
        sm = P["speed_max"]
        tk.vr = max(min(tk.vr, sm), -sm)
        va_lim = math.degrees(sm / max(tk.r, 10.0))
        tk.va = max(min(tk.va, va_lim), -va_lim)

    for fr in frames:
        t = fr["t"]
        dt = max(t - tprev, 1e-3)
        tprev = t
        warm = (t - t0) < P["warmup_s"]

        # ---- split points into channels ---- (carry el as 3rd element)
        mv = []     # (r, az, el, snr, v) moving
        st = []     # (r, az, el, snr, v) static candidates (band+snr), freshness below
        allpts = [] # (r, az) for map update
        for (r, az, el, v, snr) in fr["pts"]:
            if r < P["r_min"] or abs(az) > P["az_keep"]:
                continue
            if not (P["el_lo"] <= el <= P["el_hi"]):
                continue
            allpts.append((r, az))
            if abs(v) >= P["vmin"] and snr >= P["snr_mv"]:
                mv.append((r, az, el, snr, v))
            elif snr >= P["snr_st"]:
                st.append((r, az, el, snr, v))

        # ---- fresh-static extraction (before map update) ----
        fresh = []
        if not warm and st:
            for (r, az, el, snr, v) in st:
                ir = int((r - GR_R0) / GR_DR)
                ia = int((az - GR_A0) / GR_DA)
                if 0 <= ir < NR and 0 <= ia < NA:
                    nb = occ[max(ir-1, 0):ir+2, max(ia-1, 0):ia+2].max()
                    if nb < P["occ_free"]:
                        fresh.append((r, az, el, snr, v))
            # fresh statics must form a small cluster to count
            if fresh:
                fp = list(fresh)
                fresh = []
                for cl in _cluster(fp, P["link_r"], P["link_cross"]):
                    if len(cl) >= P["st_min_pts"]:
                        fresh.extend(fp[i] for i in cl)

        # ---- near-field flood detection ----
        caz = sorted(p[1] for p in mv if p[0] < P["flood_r"])
        if len(caz) >= P["flood_n"]:
            i10 = int(0.10 * (len(caz) - 1))
            i90 = int(0.90 * (len(caz) - 1))
            if caz[i90] - caz[i10] >= P["flood_az"]:
                flood_until = t + P["flood_hold"]

        # ---- combined measurement set: moving + fresh static ----
        sel = mv + fresh
        n_mv = len(mv)
        m = len(sel)
        if m:
            rs = np.array([p[0] for p in sel])
            azs = np.array([p[1] for p in sel])
            els = np.array([p[2] for p in sel])
            snrs = np.array([p[3] for p in sel])
            dops = np.array([p[4] for p in sel])
            is_mv = np.arange(m) < n_mv
        claimed = np.zeros(m, dtype=bool)

        # ---- predict + associate: each point goes to its NEAREST track ----
        # (normalized distance; crossing targets keep their own points instead of
        #  an elder track's big gate swallowing everything)
        preds = []
        for tk in tracks:
            pr = tk.r + tk.vr * dt
            paz = tk.az + tk.va * dt
            gcap = P["conf_grow_cap"] if tk.confirmed else P["miss_grow_cap"]
            g = min(1.0 + P["miss_grow"] * tk.misses, gcap)
            cross_speed = abs(math.radians(tk.va)) * pr
            if P["guard"] and tk.coh_bad:
                # incoherent confirmed track: its velocity fit is fed by noise,
                # so neither the speed term nor miss-growth may inflate the gate
                # (growing gates are how a ghost reaches ever-farther noise).
                g = 1.0
                rg = P["gate_r"]
                cg = P["gate_cross"]
            else:
                rg = (P["gate_r"] + abs(tk.vr) * P["speed_gate_s"]) * g
                cg = (P["gate_cross"] + cross_speed * P["speed_gate_s"]) * g
            azg = math.degrees(math.atan2(cg, max(pr, 5.0)))
            azg = min(max(azg, P["az_gate_min"] * g), P["az_gate_max"])
            preds.append((pr, paz, rg, azg))
        owner = np.full(m, -1, dtype=int)
        if m and tracks:
            # two-tier association: CONFIRMED tracks claim their points first,
            # tentative tracks compete only for the leftovers. With a dense
            # (16 dB) cloud, freshly-seeded junk otherwise steals points from
            # an established target and shreds it into short fragments.
            best_d = np.full(m, 1e9)
            for tier in (True, False):          # confirmed first
                for ti, (tk, (pr, paz, rg, azg)) in enumerate(zip(tracks, preds)):
                    if tk.confirmed is not tier:
                        continue
                    dr = np.abs(rs - pr) / rg
                    da = np.abs(azs - paz) / azg
                    d = dr + da
                    ok = (dr < 1.0) & (da < 1.0)
                    if not tier:
                        ok &= (owner == -1)     # leftovers only
                    if tk.max_mv < P["st_sustain_mv"]:
                        ok &= is_mv   # static points only sustain Doppler-proven tracks
                    upd = ok & (d < best_d)
                    owner[upd] = ti
                    best_d[upd] = d[upd]
                if tier:
                    # tentative tracks start with a clean slate on the leftovers
                    best_d = np.where(owner >= 0, -1.0, 1e9)
        for ti, tk in enumerate(tracks):
            pr, paz, rg, azg = preds[ti]
            idx = np.nonzero(owner == ti)[0]
            if len(idx):
                claimed[idx] = True
                mr = float(np.median(rs[idx]))
                maz = float(np.median(azs[idx]))
                jd = math.hypot(mr - pr, math.radians(maz - paz) * mr)
                ja = P["jit_alpha"]
                tk.jit = (1 - ja) * tk.jit + ja * jd
                tk.mv_ewma = 0.85 * tk.mv_ewma + 0.15 * float(is_mv[idx].sum())
                tk.max_mv = max(tk.max_mv, tk.mv_ewma)
                tk.st_frames = 0 if tk.mv_ewma >= P["st_mv_lo"] else tk.st_frames + 1
                a = P["alpha"]
                tk.r = a * mr + (1 - a) * pr
                aa = P["alpha_az"]
                tk.az = aa * maz + (1 - aa) * paz
                mel = float(np.median(els[idx]))       # heavy EMA on noisy el
                tk.el = (1 - P["el_alpha"]) * tk.el + P["el_alpha"] * mel
                # brightness evidence: MOVING points only (a bright static
                # return under the track proves nothing about the mover), and
                # never during a near-field flood at close range (that
                # brightness belongs to the flood, angle info is gone)
                mv_idx = idx[is_mv[idx]]
                if len(mv_idx):
                    # doppler self-consistency (vs last frame's fitted vr)
                    derr = abs(float(np.median(dops[mv_idx])) - tk.vr)
                    da_ = P["dop_alpha"]
                    tk.dop_err = (1 - da_) * tk.dop_err + da_ * derr
                    smax = float(snrs[mv_idx].max())
                    if smax > tk.snr_peak and not (
                            t < flood_until
                            and tk.r < P["flood_r"] + P["flood_margin"]):
                        tk.snr_peak = smax
                tk.hist.append((t, tk.r, tk.az))
                refit_vel(tk, t)
                tk.hits.append(1)
                tk.misses = 0
                tk.age += 1
            else:
                if (tk.confirmed and tk.moved_frames >= P["move_confirm"]
                        and (t - tk.last_moved_t) <= P["park_s"]
                        and tk.recent_motion(t, P["park_win"])[0] < P["park_disp"]):
                    tk.vr = 0.0; tk.va = 0.0        # genuinely-moved target that stopped: hold
                else:
                    tk.r, tk.az = pr, paz
                tk.hits.append(0)
                tk.misses += 1
                tk.age += 1
                tk.mv_ewma *= 0.85
                if tk.mv_ewma < P["st_mv_lo"]:
                    tk.st_frames += 1

        # ---- motion test (range-scaled, NON-latching) ----
        # A track is "moving" if its range changed, or it crossed by more than the
        # angle-noise floor at its distance, or its radial speed is real. Far
        # azimuth jitter no longer counts as motion, and nothing latches.
        for tk in tracks:
            rad, cross = tk.recent_motion(t, P["disp_win"])
            cross_floor = max(P["emit_disp"], math.radians(P["ang_floor_deg"]) * tk.r)
            tk.moving = (abs(tk.vr) >= P["emit_spd"]           # radial speed (clean, early)
                         or abs(tk.va) >= P["ang_rate_min"]    # real angular rate (early, noise-free)
                         or rad >= P["emit_disp"]              # radial displacement over window
                         or cross >= cross_floor)              # cross displacement beating noise floor
            if tk.moving:
                tk.moved_frames += 1
                tk.last_moved_t = t

        # ---- post-confirmation consistency guard ----
        # Confirmed tracks must show POSITIVE physical coherence to emit, and
        # sustained incoherence kills them (dead, not parked). Range-aware:
        #  (a) radial random walk: enough range path but net goes nowhere
        #  (b) unphysical range path speed
        #  (c) range re-latch teleports
        #  (d) association residuals (jit) beyond the az-noise floor at range
        #  (e) constant-range azimuth wander beyond the angle-noise floor
        if P["guard"]:
            glog = P.get("_guard_log")
            for tk in tracks:
                if not tk.confirmed:
                    continue
                n, r_path, r_net, r_span, c_path, c_net, tele = tk.guard_metrics(
                    t, P["guard_win"], P["guard_nseg"],
                    P["tele_spd"], P["tele_dm"] + P["tele_rk"] * tk.r,
                    P["tele_dt_max"])
                judged = n >= P["guard_min_n"] and r_span > 1e-9
                c_move_floor = max(P["pos_net"],
                                   math.radians(P["ang_floor_deg"]) * tk.r)
                if judged:
                    r_floor = P["r_path_min"] + P["r_path_rk"] * tk.r
                    r_bad = (r_path >= r_floor
                             and r_net / r_path < P["coh_min"])
                    spd_bad = r_path / r_span > P["spd_max"]
                    tele_bad = tele >= P["tele_k"]
                    c_floor = (P["cross_wander_k"]
                               * math.radians(P["ang_floor_deg"]) * tk.r)
                    c_bad = (c_path >= max(c_floor, r_floor)
                             and c_net / c_path < P["coh_min"])
                else:
                    r_bad = spd_bad = tele_bad = c_bad = False
                # only judge jitter on frames WITH evidence: during a miss
                # streak the EWMA is stale and the window is starving - the
                # miss-based lifecycle owns that case, not the guard
                jit_bad = (judged
                           and tk.jit > P["jit_base"] + P["jit_rk"] * tk.r)
                # coherent net progress per DOMAIN (above the noise floor at
                # this range): a directed mover. A coherent mover dragging its
                # gate through clutter shows jitter and flutter in the OTHER
                # domain - that is measurement extent, not a ghost re-latch.
                # KILL only on overall coherence failure (no domain coherent)
                # or on the always-hard physics tests (speed, teleports);
                # single-domain incoherence on a coherent mover only UNLATCHES
                # emission; jitter on a coherent mover is excused.
                prog_r = (judged and r_path > 0.0
                          and r_net >= P["pos_net"]
                          and r_net / r_path >= P["coh_min"])
                prog_c = (judged and c_path > 0.0
                          and c_net >= c_move_floor
                          and c_net / c_path >= P["coh_min"])
                prog = prog_r or prog_c
                hard = (spd_bad or tele_bad
                        or ((r_bad or c_bad or jit_bad) and not prog))
                soft = (not hard) and ((r_bad and not prog_r)
                                       or (c_bad and not prog_c))
                # gate freeze only on OVERALL incoherence (a coherent
                # mover's fit is real; freezing its gates while clutter
                # disturbs one domain starves it into a dropout death)
                tk.coh_bad = spd_bad or ((r_bad or c_bad) and not prog)
                if hard:
                    tk.guard_bad += 1
                else:
                    tk.guard_bad = max(tk.guard_bad - 1, 0)
                if soft:
                    tk.jit_ctr += 1
                else:
                    tk.jit_ctr = max(tk.jit_ctr - 1, 0)
                if hard or soft:
                    tk.ok_streak = 0
                else:
                    # positive evidence: coherent NET motion above the noise
                    # floor, OR standing where the latch was last held (an
                    # earned latch is re-earnable in place: walk-then-stand)
                    frozen = (t < flood_until
                              and tk.r < P["flood_r"] + P["flood_margin"])
                    standing = (tk.ever_passed
                                and abs(tk.r - tk.pass_r) < P["relatch_dr"])
                    if (judged and not frozen
                            and (r_net >= P["pos_net"]
                                 or c_net >= c_move_floor
                                 or standing)):
                        tk.ok_streak += 1
                # emission latch: positive evidence turns it on, sustained
                # trouble turns it off (re-earned with a fresh streak)
                need = (P["guard_emit_re"] if tk.ever_passed
                        else P["guard_emit"])
                if tk.ok_streak >= need:
                    tk.guard_pass = True
                    tk.ever_passed = True
                if tk.ok_streak >= P["guard_emit"]:
                    tk.deep_pass = True         # held a FULL coherent streak
                if tk.guard_pass:
                    tk.pass_r = tk.r
                if (tk.guard_bad >= P["guard_unlatch"]
                        or tk.jit_ctr >= P["guard_unlatch"]):
                    tk.guard_pass = False
                if glog is not None:
                    glog.append(dict(tid=tk.tid, t=t, n=n, r=tk.r,
                                     snrp=tk.snr_peak,
                                     r_path=r_path, r_net=r_net, r_span=r_span,
                                     c_path=c_path, c_net=c_net,
                                     tele=tele, jit=tk.jit,
                                     bad=(int(r_bad) | 2*int(spd_bad)
                                          | 4*int(tele_bad) | 8*int(c_bad)
                                          | 16*int(jit_bad))))

        # ---- lifecycle + confirmation ----
        alive = []
        for tk in tracks:
            if not tk.confirmed and tk.misses > P["tent_max_miss"]:
                continue
            if P["guard"] and tk.confirmed and tk.guard_bad >= P["guard_kill"]:
                continue                      # physically incoherent -> dead, not parked
            moved_recently = (tk.moved_frames >= P["move_confirm"]
                              and (t - tk.last_moved_t) <= P["park_s"])
            if tk.confirmed and tk.misses > P["conf_max_miss"]:
                if not (moved_recently and tk.misses <= P["park_miss"]):
                    if tk.guard_pass:
                        grave.append((t, tk.r, tk.az, tk.snr_peak))
                    continue
            # phantom cull: a confirmed track that never genuinely moved and isn't
            # moving now dies fast (this is a false-alarm cluster, not a target).
            if (tk.confirmed and not tk.moving and not moved_recently
                    and (t - tk.last_moved_t) > P["phantom_leash_s"]):
                continue
            if not tk.confirmed:
                h = list(tk.hits)
                # fast path: Doppler-backed
                if (sum(h[-P["conf_N"]:]) >= P["conf_M"]
                        and tk.mv_ewma >= P["mv_rate_min"]
                        and tk.jit < P["jit_max"]):
                    tk.confirmed = True
                # slow path: static-born / weak Doppler, needs consistency
                elif (len(h) >= P["st_conf_N"]
                        and sum(h[-P["st_conf_N"]:]) >= P["st_conf_M"]
                        and tk.jit < P["st_jit_max"]):
                    tk.confirmed = True
            alive.append(tk)
        tracks = alive
        grave = [g for g in grave if t - g[0] <= P["inherit_s"]]

        # ---- merge duplicates (keep the elder) ----
        tracks.sort(key=lambda tk: (not tk.confirmed, -tk.age))
        kill = set()
        for i in range(len(tracks)):
            a = tracks[i]
            if a.tid in kill:
                continue
            for j in range(i + 1, len(tracks)):
                b = tracks[j]
                if b.tid in kill:
                    continue
                cross = abs(math.radians(a.az - b.az)) * 0.5 * (a.r + b.r)
                if (abs(a.r - b.r) < P["merge_r"] and cross < P["merge_cross"]
                        and abs(a.vr - b.vr) < P["merge_dv"]):
                    kill.add(b.tid)
        if kill:
            tracks = [tk for tk in tracks if tk.tid not in kill]

        # ---- seed tentative tracks from unclaimed clusters ----
        if m:
            free = np.nonzero(~claimed)[0]
            if len(free):
                free = [i for i in free if is_mv[i]]   # seeds from MOVING points only
                fp = [(float(rs[i]), float(azs[i]), float(els[i]), float(snrs[i]))
                      for i in free]
                for cl in _cluster(fp, P["link_r"], P["link_cross"]):
                    cr = float(np.mean([fp[i][0] for i in cl]))
                    need = (P["seed_min_pts"] if cr >= P["seed_far_r"]
                            else P["seed_near_pts"])
                    if len(cl) < need:
                        continue
                    caz2 = float(np.mean([fp[i][1] for i in cl]))
                    cel = float(np.median([fp[i][2] for i in cl]))
                    if any(abs(tk.r - cr) < P["seed_guard_r"]
                           and abs(tk.az - caz2) < P["seed_guard_az"] for tk in tracks):
                        continue
                    tk = Track(next_id, t, cr, caz2, cel, n_win,
                               born_static=False)
                    if not (t < flood_until
                            and cr < P["flood_r"] + P["flood_margin"]):
                        tk.snr_peak = max(fp[i][3] for i in cl)
                    for (gt_, gr_, gaz_, gsnr_) in grave:
                        cross = abs(math.radians(gaz_ - caz2)) * 0.5 * (gr_ + cr)
                        if (abs(gr_ - cr) < P["inherit_r"]
                                and cross < P["inherit_cross"]):
                            tk.guard_pass = True      # re-acquired target
                            tk.ever_passed = True
                            tk.pass_r = cr
                            tk.snr_peak = max(tk.snr_peak, gsnr_)
                            break
                    tracks.append(tk)
                    sdump = P.get("_seed_dump")
                    if sdump is not None:
                        sdump.append((len(out), next_id, cr, caz2))
                    next_id += 1

        # ---- occupancy map update (frozen under live tracks) ----
        lr = P["learn_fast"] if warm else P["learn"]
        hit_cells = set()
        for (r, az) in allpts:
            ir = int((r - GR_R0) / GR_DR)
            ia = int((az - GR_A0) / GR_DA)
            if 0 <= ir < NR and 0 <= ia < NA:
                hit_cells.add((ir, ia))
        occ *= (1.0 - P["decay"])
        for (ir, ia) in hit_cells:
            occ[ir, ia] = occ[ir, ia] * (1 - lr) + lr

        # ---- emit (only genuine movers; sidelobe/duplicate suppression) ----
        cand = []
        for tk in tracks:
            if not tk.confirmed:
                continue
            if P["guard"] and not tk.guard_pass:         # no positive coherence yet
                continue
            req = min(max(P["snr_evid_hi"] - P["snr_evid_slope"] * tk.r,
                          P["snr_evid_lo"]), P["snr_evid_hi"])
            ramp = ((tk.r - P["far_margin_r0"])
                    / max(P["faint_r"] - P["far_margin_r0"], 1e-9))
            req += P["far_margin_db"] * min(max(ramp, 0.0), 1.0)
            if P["guard"] and tk.snr_peak < req:
                # faint-far relief: beyond faint_r the bar sits at the noise
                # floor and cannot separate a small target (a drone at 250 m
                # lives at 16-17 dB); a track that has held a FULL coherent
                # streak AND whose claimed doppler matches its own range-rate
                # has proven itself by physics instead.
                if not (tk.r >= P["faint_r"] and tk.deep_pass
                        and tk.dop_err <= P["dop_gate"]):
                    continue
            moved_recently = (tk.moved_frames >= P["move_confirm"]
                              and (t - tk.last_moved_t) <= P["park_s"])
            if not (tk.moving or moved_recently):        # static/phantom -> not emitted
                continue
            if tk.misses > P["emit_max_miss"] and not moved_recently:
                continue
            if tk.st_frames > P["st_emit_cap"]:
                continue
            if abs(tk.az - P["az_off"]) > P["out_az"] or tk.r > P["out_r_max"]:
                continue
            cand.append(tk)
        if P.get("_dbg"):
            _D[0] = max(_D[0], len(tracks)); _D[1] = max(_D[1], sum(1 for x in tracks if x.confirmed))
            _D[2] = max(_D[2], sum(1 for x in tracks if x.moving)); _D[3] = next_id - 1
        cand.sort(key=lambda tk: -(tk.mv_ewma + 0.001 * min(tk.age, 500)))
        emit = []
        for tk in cand:
            dup = False
            for e in emit:
                cross = abs(math.radians(e.az - tk.az)) * 0.5 * (e.r + tk.r)
                if cross < P["dedup_cross"] and abs(e.r - tk.r) < P["dedup_r"]:
                    dup = True
                    break
            if not dup:
                emit.append(tk)
        out.append([{"r": tk.r, "az": tk.az, "el": tk.el + P["el_off"],
                     "id": tk.tid} for tk in emit])
        cdump = P.get("_conf_dump")
        if cdump is not None:      # parity hook: confirmed tracks at frame end
            cdump.append([(tk.tid, tk.r, tk.az) for tk in tracks if tk.confirmed])
    if P.get("_dbg"):
        import sys; print(f"[dbg] seeded={_D[3]} max_alive={_D[0]} max_confirmed={_D[1]} max_moving={_D[2]}", file=sys.stderr)
    return out


if __name__ == "__main__":
    import sys, time, json
    from airpoc_data import load_radar, evaluate
    frames = load_radar()
    kw = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    t0 = time.time()
    pft = tracker(frames, **kw)
    print(f"tracker ran in {time.time()-t0:.1f}s  params={kw}")
    evaluate(pft)
