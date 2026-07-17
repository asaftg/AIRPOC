/* Temporal multi-target tracker — see cluster.h. Port of the offline-validated
 * radar_tracker.py (reference copy + parity harness: radar/tools). Fixed
 * arrays, no heap on the hot path.
 *
 * 2026-07-11: post-confirmation CONSISTENCY GUARD. A confirmed track must stay
 * physically coherent to live, and must show POSITIVE evidence to emit:
 *   kill   — sustained incoherence (radial random-walk, unphysical path speed,
 *            re-latch teleports, wander, jitter) kills the track (dead, not
 *            parked). Fixes the immortal wandering track (garage multipath)
 *            and ghost tracks riding the denser 16 dB point cloud.
 *   emit   — a track earns emission with a streak of judged-coherent frames
 *            showing net progress, plus BRIGHTNESS evidence: at least one
 *            claimed point with SNR >= req(range) (floor noise is ~16-21 dB at
 *            every range; a real target near the radar is far brighter, R^4).
 *   flood  — when something moves right next to the radar the sidelobes light
 *            the whole hemisphere and angle information is gone; while flooded,
 *            close tracks cannot EARN emission evidence.
 *   assoc  — two-tier association (confirmed tracks claim points first) plus a
 *            tighter miss-growth cap for confirmed tracks stops junk from
 *            shredding an established target under the open elevation diet.
 * All guard thresholds are RANGE-AWARE: range is clean at any distance while
 * azimuth noise scales as radians(err)*range. */
#include "cluster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG (M_PI/180.0)

/* ---- occupancy grid geometry (fresh-static channel) ---- */
#define NR 200         /* (400-0)/2 range cells */
#define NA 64          /* (32--32)/1 az cells   */
#define GR_R0 0.0
#define GR_DR 2.0
#define GR_A0 (-32.0)
#define GR_DA 1.0
/* ---- per-track buffers ---- */
#define HMAX 160        /* position-history ring */
#define WMAX 12         /* hit/miss window (== st_conf_N) */
#define MAX_TRK 128     /* live tracks incl. tentative (worst measured: 73) */
#define GRAVE_MAX 32    /* emission-evidence graveyard */

/* ---- fixed (non-knob) tracker params (== radar_tracker.py DEFAULTS) ---- */
/* Elevation gating is the elmax knob ONLY (symmetric, radar-frame). The old
 * hard EL_LO -9 / EL_HI +2.5 window was a level-mount ground-bench tuning:
 * on a gimbal it is wrong in every pose, and it silently blinded the tracker
 * to any airborne target above +2.5 deg. Removed 2026-07-10. */
#define AZ_KEEP_CAP 90.0     /* input az also gated by the FOV knob */
#define R_MIN 3.0
#define OCC_FREE 0.35
#define WARMUP_S 8.0
#define LEARN_FAST 0.10
#define LEARN 0.0003
#define DECAY 0.00005
#define ST_MIN_PTS 2
#define ST_SUSTAIN_MV 1.2
#define ST_MV_LO 0.35
#define GATE_R 6.0
#define GATE_CROSS 4.0
#define AZ_GATE_MIN 1.0
#define AZ_GATE_MAX 8.0
#define SPEED_GATE_S 0.45
#define MISS_GROW 0.30
#define MISS_GROW_CAP 2.0
#define CONF_GROW_CAP 1.5    /* confirmed: tighter cap on miss-driven gate growth
                              * (a big-growing gate is how a ghost reaches noise) */
#define MV_RATE_MIN 0.6
#define JIT_MAX 2.6
#define ST_CONF_M 8
#define ST_CONF_N 12
#define ST_JIT_MAX 1.8
#define JIT_ALPHA 0.35
#define TENT_MAX_MISS 3
#define EMIT_MAX_MISS 3
#define PARK_DISP 1.8
#define PARK_WIN 4.0
#define TRK_FPS 26.0     /* A/G profile frame rate: coast/park seconds -> frames */
#define VEL_WIN 0.9
#define VEL_MIN_SPAN 0.22
#define SPEED_MAX 30.0
#define ALPHA 0.55
#define ALPHA_AZ 0.7
#define EL_ALPHA 0.10
#define SEED_LINK_R 5.0
#define SEED_LINK_CROSS 3.5
#define SEED_GUARD_R 5.0
#define SEED_GUARD_AZ 2.5
#define MERGE_R 12.0
#define MERGE_CROSS 3.0
#define DEDUP_R 12.0
#define OUT_R_MAX 500.0
#define EMIT_SPD 1.2
#define EMIT_DISP 2.0
#define DISP_WIN 5.0
#define ANG_FLOOR_DEG 2.5    /* deg  angle-noise floor for cross motion/evidence */
#define ANG_RATE_MIN 3.0     /* deg/s real angular rate = a crosser */
#define MOVE_CONFIRM 8       /* frames of real motion before park-hold is earned */
#define PHANTOM_LEASH_S 1.3  /* s    confirmed track with no motion by then dies */
#define MIN_SIZE_M 0.25
#define MAX_SIZE_M 3.0
/* ---- consistency guard (radar_tracker.py "guard" block) ---- */
#define GUARD_WIN 2.5        /* s   sliding window over measured history */
#define GUARD_NSEG 5         /*     time bins for waypoint averaging */
#define GUARD_MIN_N 8        /*     measured points in window needed to judge */
#define COH_MIN 0.40         /*     net/path below this = random walk */
#define R_PATH_MIN 3.0       /* m   radial-evidence floor at r=0 ... */
#define R_PATH_RK 0.025      /* m/m ... growing with range (noise path thickens) */
#define GSPD_MAX 36.0        /* m/s range path speed above this is unphysical */
#define TELE_SPD 45.0        /* m/s implied range step speed = re-latch teleport */
#define TELE_DM 3.0          /* m   min range step to count at r=0 ... */
#define TELE_RK 0.02         /* m/m ... growing with range (multipath mode flips) */
#define TELE_DT_MAX 0.5      /* s   only judge steps without a long gap */
#define TELE_K 3             /*     teleports in window to flag */
#define JIT_BASE 3.5         /* m   innovation EWMA allowance at r=0 ... */
#define JIT_RK 0.02          /* m/m ... growing with range */
#define CROSS_WANDER_K 2.0   /*     cross path floor factor for wander test */
#define GUARD_KILL 10        /*     bad-frame counter (hysteresis) -> kill */
#define GUARD_EMIT 12        /*     judged-good streak to earn first emission */
#define GUARD_EMIT_RE 3      /*     shorter streak to re-latch a proven target */
#define GUARD_UNLATCH 5      /*     counter level that unlatches emission */
#define POS_NET 2.0          /* m   net window progress = positive evidence */
/* brightness evidence: to emit at range r a track's lifetime PEAK claimed SNR
 * must clear req(r) = clamp(HI - SLOPE*r, LO, HI) for its CURRENT range (not a
 * latch: a far-born floor-noise track that wanders close faces a bar it can
 * never meet; a real approacher brightens R^4-fast). Floor-noise ghosts peak
 * 18-21 dB at 15-120 m, real tracks 21+ dB even at 250 m. */
#define SNR_EVID_HI 24.5
#define SNR_EVID_LO 17.5
#define SNR_EVID_SLOPE 0.025
/* near-field flood: moving cloud below FLOOD_R covering FLOOD_AZ of azimuth */
#define FLOOD_R 40.0
#define FLOOD_N 25
#define FLOOD_AZ 80.0
#define FLOOD_HOLD 2.0
#define FLOOD_MARGIN 10.0
/* emission-evidence handoff (re-acquired target after a dropout death) */
#define INHERIT_S 2.0
#define INHERIT_R 10.0
#define INHERIT_CROSS 6.0
/* ship-gate review fixes (2026-07-11):
 *  - RELATCH_DR: an earned latch is re-earnable IN PLACE (walk-then-stand far
 *    target keeps its box; a never-latched wanderer gains nothing by standing)
 *  - FAINT_R/FAR_MARGIN: beyond FAINT_R the brightness bar sits at the noise
 *    floor, so passing it outright needs a margin (real far tracks measure
 *    >= +2.6 dB over the bar, spur streaks +1.5); below the margin the faint
 *    path is the only way to emit: a FULL coherent streak plus DOPPLER SELF-
 *    CONSISTENCY (a real mover's claimed points carry doppler matching its
 *    fitted range-rate, p50 err 0.9-3.3 m/s; a spur-comb streak's apparent
 *    motion comes from re-latching, its point doppler is soup, p50 16-19).
 *    Caveat: a fast radial target beyond the doppler fold fails the faint
 *    path; acceptable for the faint-far (slow/small) case. */
#define RELATCH_DR 15.0
#define FAINT_R 200.0
#define FAR_MARGIN_DB 2.0   /* ramps in over FAR_MARGIN_R0..FAINT_R (no cliff) */
#define FAR_MARGIN_R0 150.0
#define DOP_GATE 4.0
#define DOP_ALPHA 0.2
/* ---- doppler-walk evidence (2026-07-16): does the claim match the walk? --
 * A real target's claimed doppler (which on this firmware is its range-rate,
 * sign verified on T7) integrates to the range displacement it actually
 * walks. Over the trailing WALK_WIN of MEASURED history:
 *   D  = trapezoid integral of the per-hit-frame median claimed doppler
 *        (gaps > WALK_GAP_MAX not integrated; per-frame claims clamped to
 *        WALK_DOP_CAPK x the window median so one noise-point median cannot
 *        race D ahead);
 *   dR = SIGNED net range displacement from time-binned waypoints over the
 *        SAME covered pairs, each pair's range step winsorized against its
 *        own claim (an association re-latch jump must not count as motion
 *        the target never claimed).
 * Instantaneous versions of this test were tried and measured useless at
 * long range: |claimed - fitted vr| runs 5-15 m/s on the GENUINE T7 far
 * human (the fit chases azimuth noise and re-latches), so only windowed
 * integrals separate real from ghost. Consumers below: the liar kill
 * (LIAR_*, this commit) and the walk guard (planned re-add). */
#define WALK_WIN 5.0         /* s    verification window */
#define WALK_NSEG 6          /*      time bins for the waypoint fit */
#define WALK_GAP_MAX 0.5     /* s    do not integrate doppler across gaps */
#define WALK_DOP_MIN 1.2     /* m/s  median-claim floor for the clamp scale */
#define WALK_D_MIN 3.0       /* m    |integrated D| to be decidable */
#define WALK_COV_MIN_S 1.5   /* s    integrated (covered) time to be decidable */
#define WALK_DOP_CAPK 3.0    /*      per-frame |claimed dop| clamp: K*median */
#define WALK_JUMP_K 3.0      /*      per-pair |dr| cap: K*|claimed dop|*dt ... */
#define WALK_JUMP_M 0.5      /* m/s  ... + this slack rate */
#define WALK_TOL_M 1.5       /* m    |dR - D| tolerance at r=0 ... */
#define WALK_TOL_RK 0.01     /* m/m  ... growing with range (endpoint noise) */
#define WALK_TOL_K 0.3       /*      ... or this fraction of |D| */
/* ---- liar latch (2026-07-16): starved claims = far-clutter ghost ----
 * A real mover past 100 m owns integrable claimed doppler nearly every
 * frame: the T7 walker's covered time tracks its full window (measured).
 * The radar4 far-clutter ghost does not: its apparent motion comes from
 * re-latching, and it only touches doppler in soup bursts — measured
 * covered time 0.04-1.4 s inside a FULL 5 s window of position history,
 * with wild claim medians (1.2-31 m/s frame to frame). Strike per evidence
 * frame (fresh claimed doppler this frame), confirmed tracks past
 * LIAR_R_MIN only: a full history window (span >= LIAR_SPAN_MIN) whose
 * integrable doppler coverage stays under WALK_COV_MIN_S while the claims
 * it does make say "mover" (median >= WALK_DOP_MIN). A healthy-coverage
 * frame pays one strike back (guard_bad-style hysteresis); LIAR_KILL net
 * strikes LATCH the track as a liar: it keeps living and claiming its
 * points (so its junk cannot re-seed a fresh ghost every few seconds) but
 * it never reaches the wire again, and its coast-death leaves no graveyard
 * credential — a ghost must not will its emission rights to the next
 * ghost. Suppression, not death, is deliberate: KILLING these tracks was
 * tried and measured to RAISE radar4 emissions 43% — every kill spawned a
 * successor chain and reshuffled emit dedup across the whole scene, while
 * the latch leaves tracker dynamics bit-identical outside the liar itself.
 * Never judged below LIAR_R_MIN: near-field multipath makes claimed
 * doppler soup, and radar4's real walkers live there.
 * Alternates tried and measured before this test (kept honest):
 *   - |claimed - fitted vr| EWMA bar (1.5 m/s, fold-corrected): unusable —
 *     the genuine T7 far human runs 5-15 m/s of fit error at 250 m+,
 *     indistinguishable from the ghosts by magnitude;
 *   - claimed-vs-drift DIRECTION contradiction on the walk integrals:
 *     safe and it kills the re-latch ladder class (T7 tid11033 climbing
 *     181->257 m at an implied 63 m/s against its own claims), but that
 *     class is the walk guard's job (planned re-add), and the extra kills
 *     reshuffled V2DAY over its frozen baseline. Not included. */
#define LIAR_R_MIN 100.0     /* m    never judge below this (near-field soup) */
#define LIAR_SPAN_MIN 4.0    /* s    history span that makes starvation chronic */
#ifndef LIAR_KILL
#define LIAR_KILL 13         /*      net striking evidence frames -> latch (~0.5 s) */
#endif
/* V_FOLD from the shipped A/G cfg (profileCfg idle 3 us + rampEnd 20.5 us =
 * 23.5 us chirp repeat at 77 GHz): v = lambda/(4*Tc) = 41.4 m/s. Matches the
 * measured max |doppler| in the corpus (41.416). Re-derive if the cfg chirp
 * timing ever changes. */
#define V_FOLD_MPS 41.45
/* ---- reflection-copy suppressor (2026-07-16): sustained co-range shadow --
 * An antenna sidelobe shows a bright mover a SECOND time: same range (bin-
 * identical lockstep), same signed doppler, azimuth 10-70 deg off (measured
 * radar5 — the garage walker and car each drag such copies). At the emit
 * stage, a candidate standing in a stronger emitted target's co-range +
 * co-velocity shadow is put on watch. Time is what convicts: real tracks
 * crossing each other's range stay co-ranged ~1-2 s (measured on the T1/T2
 * crossing pairs — naive same-frame suppression wrongly ate 94/29 of their
 * frames), while a reflection shadows its source for its whole life, so
 * only a shadow held >= REFL_SHADOW_S suppresses. Below that the candidate
 * still emits, flagged suspect on the wire ("sus":1) so fusion can hold
 * fire without the radar hiding anything.
 * Bookkeeping runs for EVERY confirmed track, not just emit candidates: a
 * copy shadows its source while it is still earning emission (measured
 * radar5: the mirror is co-ranged for seconds before its first emit), so
 * by the time it asks for the wire its sentence is already served. The
 * shadow clock only resets on a DECISIVE separation (>= REFL_CLEAR_S
 * continuously clear): single-frame match blips from measurement noise
 * must not launder a copy, while a genuine crosser separates in range and
 * stays separated. The velocity match is SIGNED and wrap-aware
 * (V_FOLD_MPS): V2DAY's opposite-direction vehicle pairs (-4.3 vs
 * +4.4 m/s) meet at the same range but never match. The separation floor
 * keeps this out of the spatial dedup's territory: a shadow only counts
 * when the copy is FAR in cross-range yet glued in range+velocity —
 * physically a sidelobe, never a split cluster of the same body. */
#define REFL_R 5.0           /* m    co-range gate |S.r - C.r| */
#define REFL_DV 1.5          /* m/s  SIGNED co-velocity gate (wrap-aware) */
#define REFL_SEP_M 4.5       /* m    min cross-range separation to be a copy ... */
#define REFL_SEP_DEG 10.0    /* deg  ... or this azimuth arc, whichever larger */
#define REFL_SHADOW_S 2.5    /* s    sustained shadow that convicts a copy */
#define REFL_CLEAR_S 1.0     /* s    continuous clear that resets the clock */

typedef struct {
    int used, tid;
    double r, az, el, vr, va;
    double jit, mv_ewma, max_mv;
    int misses, age, st_frames, hits_total;
    int confirmed;
    double ht[HMAX], hr[HMAX], ha[HMAX]; int hn, hhead;
    double hd[HMAX];         /* median claimed doppler per hit frame (NAN: none) */
    int hit[WMAX], hitn;
    double sx, sy, sz;           /* half-extents (m), from the frame's points */
    /* motion test */
    int moved_frames, moving;
    double last_moved_t;
    /* consistency guard */
    int guard_bad, ok_streak, guard_pass, ever_passed, coh_bad;
    int jit_ctr, deep_pass;  /* soft-trouble counter; held a FULL streak once */
    double pass_r;           /* range where the latch was last held */
    double dop_err;          /* EWMA |median claimed doppler - fitted vr| */
    int liar_evid, liar_bad; /* liar latch: fresh-doppler flag, strike counter */
    int liar;                /* latched ghost: lives on, never emitted */
    double shadow_s;         /* time spent in a stronger target's co-range
                                co-velocity shadow (reflection suppressor) */
    double clear_s;          /* continuous time out of any shadow */
    int breeder;             /* a copy already served full time in THIS
                                track's shadow: proven mirror source, its
                                later copies convict instantly */
    double snr_peak;         /* lifetime max claimed SNR (brightness evidence) */
    int snr_unknown;         /* saw a point without SNR (no TLV7) -> fail open */
} Track;

typedef struct { double t, r, az, snr_peak; } Grave;

struct RadarClusterer {
    Track tracks[MAX_TRK];
    int   ord[MAX_TRK];  /* live slots in python-list order (assoc/merge/emit) */
    int   nord;
    int   next_tid;
    float occ[NR][NA];
    double t0; int have_t0;
    double flood_until;
    Grave grave[GRAVE_MAX]; int ngrave;
    /* live knobs */
    float dedup_cross;   /* eps knob   */
    int   min_pts;       /* min_pts    */
    float vmin;          /* speed knob */
    float snr_mv;        /* snr knob (static = +3) */
    float fov_half;      /* fov knob   */
    float el_max;        /* elmax knob (elevation half-angle, 90 = off) */
    float merge_dv;      /* doppler knob */
    int   conf_m;        /* confirm knob (M-of-N, window = M+1) */
    double coast_s;      /* coast knob (seconds) */
    double park_s;       /* park-hold knob (seconds) */
    int dbg_nmv, dbg_m;  /* bench introspection */
};

RadarClusterer *cluster_new(void) {
    RadarClusterer *c = calloc(1, sizeof(RadarClusterer));
    if (c) {
        c->next_tid    = 1;
        c->dedup_cross = (float)CLUSTER_DEFAULT_EPS_M;
        c->min_pts     = CLUSTER_DEFAULT_MIN_PTS;
        c->vmin        = (float)CLUSTER_DEFAULT_SPEED;
        c->snr_mv      = (float)CLUSTER_DEFAULT_SNR;
        c->fov_half    = (float)CLUSTER_DEFAULT_FOV;
        c->el_max      = (float)CLUSTER_DEFAULT_ELMAX;
        c->merge_dv    = (float)CLUSTER_DEFAULT_DOP;
        c->conf_m      = CLUSTER_DEFAULT_CONFIRM;
        c->coast_s     = CLUSTER_DEFAULT_COAST_S;
        c->park_s      = CLUSTER_DEFAULT_PARK_S;
        c->flood_until = -1e18;
    }
    return c;
}
void cluster_free(RadarClusterer *c) { free(c); }
double cluster_fov(const RadarClusterer *c) { return c ? c->fov_half : CLUSTER_DEFAULT_FOV; }

void cluster_set_dbscan(RadarClusterer *c, double eps_m, int min_pts) {
    if (!c) return;
    if (eps_m < CLUSTER_EPS_MIN_M) eps_m = CLUSTER_EPS_MIN_M;
    if (eps_m > CLUSTER_EPS_MAX_M) eps_m = CLUSTER_EPS_MAX_M;
    if (min_pts < CLUSTER_MIN_PTS_MIN) min_pts = CLUSTER_MIN_PTS_MIN;
    if (min_pts > CLUSTER_MIN_PTS_MAX) min_pts = CLUSTER_MIN_PTS_MAX;
    c->dedup_cross = (float)eps_m; c->min_pts = min_pts;
}
void cluster_set_gates(RadarClusterer *c, double speed_min_mps, double snr_min_db,
                       double fov_half_deg, double el_max_deg, double doppler_gate_mps) {
    if (!c) return;
    if (speed_min_mps < CLUSTER_SPEED_MIN) speed_min_mps = CLUSTER_SPEED_MIN;
    if (speed_min_mps > CLUSTER_SPEED_MAX) speed_min_mps = CLUSTER_SPEED_MAX;
    if (snr_min_db < CLUSTER_SNR_MIN) snr_min_db = CLUSTER_SNR_MIN;
    if (snr_min_db > CLUSTER_SNR_MAX) snr_min_db = CLUSTER_SNR_MAX;
    if (fov_half_deg < CLUSTER_FOV_MIN) fov_half_deg = CLUSTER_FOV_MIN;
    if (fov_half_deg > CLUSTER_FOV_MAX) fov_half_deg = CLUSTER_FOV_MAX;
    if (el_max_deg < CLUSTER_ELMAX_MIN) el_max_deg = CLUSTER_ELMAX_MIN;
    if (el_max_deg > CLUSTER_ELMAX_MAX) el_max_deg = CLUSTER_ELMAX_MAX;
    if (doppler_gate_mps < CLUSTER_DOP_MIN) doppler_gate_mps = CLUSTER_DOP_MIN;
    if (doppler_gate_mps > CLUSTER_DOP_MAX) doppler_gate_mps = CLUSTER_DOP_MAX;
    c->vmin = (float)speed_min_mps; c->snr_mv = (float)snr_min_db;
    c->fov_half = (float)fov_half_deg; c->el_max = (float)el_max_deg;
    c->merge_dv = (float)doppler_gate_mps;
}
void cluster_set_track(RadarClusterer *c, int confirm_m, double coast_s, double park_s) {
    if (!c) return;
    if (confirm_m < CLUSTER_CONFIRM_MIN) confirm_m = CLUSTER_CONFIRM_MIN;
    if (confirm_m > CLUSTER_CONFIRM_MAX) confirm_m = CLUSTER_CONFIRM_MAX;
    if (coast_s < CLUSTER_COAST_MIN) coast_s = CLUSTER_COAST_MIN;
    if (coast_s > CLUSTER_COAST_MAX) coast_s = CLUSTER_COAST_MAX;
    if (park_s < CLUSTER_PARK_MIN) park_s = CLUSTER_PARK_MIN;
    if (park_s > CLUSTER_PARK_MAX) park_s = CLUSTER_PARK_MAX;
    c->conf_m = confirm_m; c->coast_s = coast_s; c->park_s = park_s;
}

/* ---- helpers ---- */
static double clampd(double v, double lo, double hi){ return v<lo?lo:(v>hi?hi:v); }
static int dcmp(const void *a, const void *b){ double d=*(const double*)a-*(const double*)b; return (d>0)-(d<0); }
static double med(const double *v, int n){
    static double a[RADAR_MAX_POINTS]; memcpy(a, v, (size_t)n*sizeof(double));
    qsort(a, (size_t)n, sizeof(double), dcmp);
    return (n&1) ? a[n/2] : 0.5*(a[n/2-1]+a[n/2]);
}
static void hist_push(Track *t, double tm, double r, double a, double dop){
    t->ht[t->hhead]=tm; t->hr[t->hhead]=r; t->ha[t->hhead]=a; t->hd[t->hhead]=dop;
    t->hhead=(t->hhead+1)%HMAX; if(t->hn<HMAX) t->hn++;
}
/* copy the in-window tail of the history ring in CHRONOLOGICAL order */
static int hist_window(const Track *t, double tnow, double win,
                       double *ht, double *hr, double *ha){
    int n=0;
    for(int i=0;i<t->hn;i++){
        int idx=(t->hhead - t->hn + i + HMAX)%HMAX;      /* oldest -> newest */
        if(tnow - t->ht[idx] <= win){ ht[n]=t->ht[idx]; hr[n]=t->hr[idx]; ha[n]=t->ha[idx]; n++; }
    }
    return n;
}
static void hit_push(Track *t, int v){
    if(t->hitn<WMAX) t->hit[t->hitn++]=v;
    else { memmove(t->hit, t->hit+1, (WMAX-1)*sizeof(int)); t->hit[WMAX-1]=v; }
}
static int hit_sum_last(Track *t, int k){
    int s=0, n=t->hitn; if(k>n)k=n;
    for(int i=n-k;i<n;i++) s+=t->hit[i];
    return s;
}
/* (radial, cross) coherent displacement over the window: first-third vs
 * last-third mean of MEASURED history (radar_tracker.py recent_motion) */
static void recent_motion(const Track *t, double tnow, double win,
                          double *rad, double *cross){
    double ht[HMAX], hr[HMAX], ha[HMAX];
    int n = hist_window(t, tnow, win, ht, hr, ha);
    *rad = 0.0; *cross = 0.0;
    if(n<9) return;
    int k=n/3; double r1=0,r2=0,a1=0,a2=0;
    for(int i=0;i<k;i++){ r1+=hr[i]; a1+=ha[i]; }
    for(int i=n-k;i<n;i++){ r2+=hr[i]; a2+=ha[i]; }
    r1/=k; a1/=k; r2/=k; a2/=k;
    *rad = fabs(r2-r1);
    *cross = fabs((a2-a1)*DEG)*0.5*(r1+r2);
}
/* Doppler-walk evidence over the trailing WALK_WIN of MEASURED history (see
 * WALK_* block above). Returns the number of in-window samples; outputs the
 * doppler displacement integral D and the SIGNED measured range displacement
 * dR over EXACTLY the integrated pairs (telescoped per contiguous run — the
 * two must be compared over the same covered time, or a flickery far track
 * fails by construction), the covered time t_cov, the waypoint cross-range
 * net/path (m), and the median |claimed doppler| over covered hit frames. */
static int walk_metrics(const Track *t, double tnow, double *D, double *dR,
                        double *t_cov, double *c_net, double *c_path,
                        double *meddop){
    double ht[HMAX], hr[HMAX], ha[HMAX], hd[HMAX];
    int n=0;
    for(int i=0;i<t->hn;i++){
        int idx=(t->hhead - t->hn + i + HMAX)%HMAX;      /* oldest -> newest */
        if(tnow - t->ht[idx] <= WALK_WIN){
            ht[n]=t->ht[idx]; hr[n]=t->hr[idx]; ha[n]=t->ha[idx]; hd[n]=t->hd[idx]; n++;
        }
    }
    *D=*dR=*t_cov=*c_net=*c_path=0.0; *meddop=0.0;
    if(n<2) return n;
    /* doppler displacement integral (trapezoid; no integration across gaps)
     * + measured range displacement over the SAME pairs. Both robustified:
     * per-frame claimed doppler is clamped to WALK_DOP_CAPK x the window
     * median (one 30 m/s noise-point median on a 1.8 m/s walker must not
     * race D ahead — the integral is mean-like), and each pair's range step
     * is winsorized against its own claim (an association re-latch jump of
     * +6 m in one 0.4 s gap, T2 @ 217 m, is measurement artifact, not
     * motion a ghost gets credit for). */
    static double ad[HMAX]; int nd=0;
    for(int i=0;i<n;i++) if(!isnan(hd[i])) ad[nd++]=fabs(hd[i]);
    if(nd) *meddop=med(ad,nd);
    double dcap = WALK_DOP_CAPK * (*meddop > WALK_DOP_MIN ? *meddop : WALK_DOP_MIN);
    for(int i=1;i<n;i++){
        double sdt=ht[i]-ht[i-1];
        if(sdt<=0.0 || sdt>WALK_GAP_MAX) continue;
        if(isnan(hd[i]) || isnan(hd[i-1])) continue;
        double d0=clampd(hd[i-1],-dcap,dcap), d1=clampd(hd[i],-dcap,dcap);
        *D += 0.5*(d0+d1)*sdt;
        {
            double dmx=fabs(d1)>fabs(d0)?fabs(d1):fabs(d0);
            double cap=(WALK_JUMP_K*dmx + WALK_JUMP_M)*sdt;
            *dR += clampd(hr[i]-hr[i-1], -cap, cap);
        }
        *t_cov += sdt;
    }
    /* signed waypoint displacement (guard_metrics-style time binning) */
    double tlo=ht[0];
    double seg_dt=(ht[n-1]-tlo)/WALK_NSEG; if(seg_dt<1e-9) seg_dt=1e-9;
    double wr[WALK_NSEG+1], wa[WALK_NSEG+1]; int nwp=0;
    double br=0,ba=0; int bc=0, cur=0;
    for(int i=0;i<n;i++){
        int k=(int)((ht[i]-tlo)/seg_dt); if(k>WALK_NSEG-1) k=WALK_NSEG-1;
        if(k!=cur && bc){
            wr[nwp]=br/bc; wa[nwp]=ba/bc; nwp++;
            br=ba=0; bc=0; cur=k;
        }
        br+=hr[i]; ba+=ha[i]; bc++;
    }
    if(bc){ wr[nwp]=br/bc; wa[nwp]=ba/bc; nwp++; }
    if(nwp<2) return n;
    *dR = wr[nwp-1]-wr[0];                                /* SIGNED */
    *c_net = fabs((wa[nwp-1]-wa[0])*DEG)*0.5*(wr[0]+wr[nwp-1]);
    for(int i=1;i<nwp;i++)
        *c_path += fabs((wa[i]-wa[i-1])*DEG)*0.5*(wr[i]+wr[i-1]);
    return n;
}
static void refit_vel(Track *t, double tnow){
    double tt[HMAX], rr[HMAX], aa[HMAX];
    int n = hist_window(t, tnow, VEL_WIN, tt, rr, aa);
    if(n<3) return;
    if(tt[n-1]-tt[0] < VEL_MIN_SPAN) return;
    double mt=0,mr=0,ma=0; for(int i=0;i<n;i++){ mt+=tt[i]; mr+=rr[i]; ma+=aa[i]; } mt/=n; mr/=n; ma/=n;
    double den=0,nr=0,na=0;
    for(int i=0;i<n;i++){ double dm=tt[i]-mt; den+=dm*dm; nr+=dm*(rr[i]-mr); na+=dm*(aa[i]-ma); }
    if(den<=0) return;
    t->vr=clampd(nr/den, -SPEED_MAX, SPEED_MAX);
    double valim=(SPEED_MAX/(t->r>10.0?t->r:10.0))/DEG;
    t->va=clampd(na/den, -valim, valim);
}
/* physical-coherence evidence over the trailing window of MEASURED positions
 * (radar_tracker.py guard_metrics): time-binned waypoints suppress per-frame
 * noise; teleports count raw impossible range steps. */
static int guard_metrics(const Track *t, double tnow, double tele_dm,
                         double *r_path, double *r_net, double *r_span,
                         double *c_path, double *c_net, int *tele){
    double ht[HMAX], hr[HMAX], ha[HMAX];
    int n = hist_window(t, tnow, GUARD_WIN, ht, hr, ha);
    *r_path=*r_net=*r_span=*c_path=*c_net=0.0; *tele=0;
    if(n<2) return n;
    for(int i=1;i<n;i++){
        double sdt=ht[i]-ht[i-1], dr=fabs(hr[i]-hr[i-1]);
        if(sdt>0.0 && sdt<=TELE_DT_MAX && dr>tele_dm && dr/sdt>TELE_SPD) (*tele)++;
    }
    double tlo=ht[0];
    double seg_dt=(ht[n-1]-tlo)/GUARD_NSEG; if(seg_dt<1e-9) seg_dt=1e-9;
    double wt[GUARD_NSEG+1], wr[GUARD_NSEG+1], wa[GUARD_NSEG+1]; int nwp=0;
    double bt=0,br=0,ba=0; int bc=0, cur=0;
    for(int i=0;i<n;i++){
        int k=(int)((ht[i]-tlo)/seg_dt); if(k>GUARD_NSEG-1) k=GUARD_NSEG-1;
        if(k!=cur && bc){
            wt[nwp]=bt/bc; wr[nwp]=br/bc; wa[nwp]=ba/bc; nwp++;
            bt=br=ba=0; bc=0; cur=k;
        }
        bt+=ht[i]; br+=hr[i]; ba+=ha[i]; bc++;
    }
    if(bc){ wt[nwp]=bt/bc; wr[nwp]=br/bc; wa[nwp]=ba/bc; nwp++; }
    if(nwp<3) return n;
    for(int i=1;i<nwp;i++){
        *r_path += fabs(wr[i]-wr[i-1]);
        *c_path += fabs((wa[i]-wa[i-1])*DEG)*0.5*(wr[i]+wr[i-1]);
    }
    *r_net = fabs(wr[nwp-1]-wr[0]);
    *c_net = fabs((wa[nwp-1]-wa[0])*DEG)*0.5*(wr[0]+wr[nwp-1]);
    *r_span = wt[nwp-1]-wt[0];
    return n;
}
/* flood-fill cluster on (r,az); fills label[], returns ncluster */
static int cluster_pts(const double *r, const double *az, int n,
                       double link_r, double link_cross, int *label){
    for(int i=0;i<n;i++) label[i]=-1;
    static int stk[RADAR_MAX_POINTS]; int nc=0;
    for(int i=0;i<n;i++){
        if(label[i]>=0) continue;
        int sp=0; stk[sp++]=i; label[i]=nc;
        while(sp){
            int j=stk[--sp];
            for(int k=0;k<n;k++){
                if(label[k]>=0) continue;
                if(fabs(r[k]-r[j])<link_r){
                    double cross=fabs((az[k]-az[j])*DEG)*0.5*(r[k]+r[j]);
                    if(cross<link_cross){ label[k]=nc; stk[sp++]=k; }
                }
            }
        }
        nc++;
    }
    return nc;
}
static double snr_req(double r){
    return clampd(SNR_EVID_HI - SNR_EVID_SLOPE*r, SNR_EVID_LO, SNR_EVID_HI);
}
static void ord_remove(RadarClusterer *R, int pos){
    R->tracks[R->ord[pos]].used = 0;
    memmove(R->ord+pos, R->ord+pos+1, (size_t)(R->nord-pos-1)*sizeof(int));
    R->nord--;
}

int cluster_step(RadarClusterer *R, RadarPoint *pts, int n,
                 double now_t, double dt, RadarTarget *out, int max_out) {
    if (dt <= 0) dt = 0.05;
    if (!R->have_t0){ R->t0 = now_t; R->have_t0 = 1; R->flood_until = now_t - 1.0; }
    int warm = (now_t - R->t0) < WARMUP_S;
    double snr_st = R->snr_mv + 3.0f;
    double fov = R->fov_half; if (fov > AZ_KEEP_CAP) fov = AZ_KEEP_CAP;
    double el_max = R->el_max;                            /* live elmax knob   */
    int conf_m = R->conf_m, conf_n = conf_m + 1;          /* live confirm knob */
    int coast_frames = (int)lround(R->coast_s * TRK_FPS); /* live coast knob   */
    int park_frames  = (int)lround(R->park_s * TRK_FPS);  /* live park knob    */

    for (int i=0;i<n;i++) pts[i].tid = 255;

    /* ---- channel split (srcpt keeps pts index for tid tagging) ---- */
    static double rs[RADAR_MAX_POINTS], azs[RADAR_MAX_POINTS], els[RADAR_MAX_POINTS], snrs[RADAR_MAX_POINTS], dops[RADAR_MAX_POINTS];
    static int ismv[RADAR_MAX_POINTS], srcpt[RADAR_MAX_POINTS], snrok[RADAR_MAX_POINTS];
    static double ar[RADAR_MAX_POINTS], aa_[RADAR_MAX_POINTS];
    int m=0, nmv=0, nall=0;
    static int st_idx[RADAR_MAX_POINTS];
    static double st_r[RADAR_MAX_POINTS], st_a[RADAR_MAX_POINTS], st_e[RADAR_MAX_POINTS], st_s[RADAR_MAX_POINTS];
    int nst=0;
    for (int i=0;i<n;i++){
        double r=pts[i].range, az=pts[i].az, el=pts[i].el, v=pts[i].doppler, snr=pts[i].snr;
        if (r<R_MIN || fabs(az)>fov || fabs(el)>el_max) continue;
        ar[nall]=r; aa_[nall]=az; nall++;
        int snr_known = !isnan(snr);
        if (fabs(v)>=R->vmin && (!snr_known || snr>=R->snr_mv)) {
            rs[m]=r; azs[m]=az; els[m]=el; snrs[m]=snr_known?snr:0.0;
            dops[m]=v;
            snrok[m]=snr_known; ismv[m]=1; srcpt[m]=i; m++; nmv++;
        } else if (snr_known && snr>=snr_st) {
            st_idx[nst]=i; st_r[nst]=r; st_a[nst]=az; st_e[nst]=el; st_s[nst]=snr; nst++;
        }
    }
    /* ---- near-field flood detection: moving cloud below FLOOD_R spanning
     * most of the azimuth axis = something is right next to the radar and
     * angle information is physically gone (sidelobe hemisphere). ---- */
    {
        static double caz[RADAR_MAX_POINTS]; int ncz=0;
        for(int i=0;i<nmv;i++) if(rs[i]<FLOOD_R) caz[ncz++]=azs[i];
        if(ncz>=FLOOD_N){
            qsort(caz,(size_t)ncz,sizeof(double),dcmp);
            int i10=(int)(0.10*(ncz-1)), i90=(int)(0.90*(ncz-1));
            if(caz[i90]-caz[i10] >= FLOOD_AZ) R->flood_until = now_t + FLOOD_HOLD;
        }
    }
    /* ---- fresh-static: high-SNR returns in historically-empty cells ---- */
    if (!warm && nst){
        static double cr[RADAR_MAX_POINTS], ca[RADAR_MAX_POINTS], ce[RADAR_MAX_POINTS], cs_[RADAR_MAX_POINTS];
        static int cidx[RADAR_MAX_POINTS]; int nc=0;
        for (int i=0;i<nst;i++){
            int ir=(int)((st_r[i]-GR_R0)/GR_DR), ia=(int)((st_a[i]-GR_A0)/GR_DA);
            if (ir>=0&&ir<NR&&ia>=0&&ia<NA){
                float nb=0; int i0=ir-1<0?0:ir-1,i1=ir+1>=NR?NR-1:ir+1,j0=ia-1<0?0:ia-1,j1=ia+1>=NA?NA-1:ia+1;
                for(int a=i0;a<=i1;a++) for(int b=j0;b<=j1;b++) if(R->occ[a][b]>nb) nb=R->occ[a][b];
                if (nb<OCC_FREE){ cr[nc]=st_r[i]; ca[nc]=st_a[i]; ce[nc]=st_e[i]; cs_[nc]=st_s[i]; cidx[nc]=st_idx[i]; nc++; }
            }
        }
        if (nc){
            static int lbl[RADAR_MAX_POINTS]; int k=cluster_pts(cr,ca,nc,SEED_LINK_R,SEED_LINK_CROSS,lbl);
            static int cnt[RADAR_MAX_POINTS]; for(int c=0;c<k;c++) cnt[c]=0;
            for(int i=0;i<nc;i++) cnt[lbl[i]]++;
            for(int i=0;i<nc;i++) if(cnt[lbl[i]]>=ST_MIN_PTS && m<RADAR_MAX_POINTS){
                rs[m]=cr[i]; azs[m]=ca[i]; els[m]=ce[i]; snrs[m]=cs_[i];
                dops[m]=0.0;
                snrok[m]=1; ismv[m]=0; srcpt[m]=cidx[i]; m++;
            }
        }
    }

    R->dbg_nmv = nmv; R->dbg_m = m;
    /* ---- predictions (python-list order) ---- */
    static double PR[MAX_TRK], PAZ[MAX_TRK], RG[MAX_TRK], AZG[MAX_TRK];
    for (int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        double pr=t->r+t->vr*dt, paz=t->az+t->va*dt;
        double gcap = t->confirmed ? CONF_GROW_CAP : MISS_GROW_CAP;
        double g=1.0+MISS_GROW*t->misses; if(g>gcap)g=gcap;
        double rg, cg;
        if (t->coh_bad){
            /* incoherent confirmed track: its velocity fit is fed by noise, so
             * neither the speed term nor miss-growth may inflate the gate. */
            g = 1.0; rg = GATE_R; cg = GATE_CROSS;
        } else {
            double cs=fabs(t->va*DEG)*pr;
            rg=(GATE_R+fabs(t->vr)*SPEED_GATE_S)*g;
            cg=(GATE_CROSS+cs*SPEED_GATE_S)*g;
        }
        double azg=atan2(cg, pr>5.0?pr:5.0)/DEG;
        double lo=AZ_GATE_MIN*g; if(azg<lo)azg=lo; if(azg>AZ_GATE_MAX)azg=AZ_GATE_MAX;
        int ti=R->ord[oi];
        PR[ti]=pr; PAZ[ti]=paz; RG[ti]=rg; AZG[ti]=azg;
    }
    /* ---- two-tier per-point nearest-track association: confirmed tracks
     * claim their points first; tentative tracks compete for leftovers ---- */
    static int owner[RADAR_MAX_POINTS], tier1[RADAR_MAX_POINTS];
    static double bestd[RADAR_MAX_POINTS];
    for(int i=0;i<m;i++){ owner[i]=-1; bestd[i]=1e9; }
    for(int tier=1;tier>=0;tier--){
        for(int oi=0;oi<R->nord;oi++){
            int ti=R->ord[oi]; Track *t=&R->tracks[ti];
            if(t->confirmed != tier) continue;
            int st_ok = t->max_mv >= ST_SUSTAIN_MV;
            for(int i=0;i<m;i++){
                if(!tier && tier1[i]) continue;          /* leftovers only */
                if(!ismv[i] && !st_ok) continue;
                double dr=fabs(rs[i]-PR[ti])/RG[ti], da=fabs(azs[i]-PAZ[ti])/AZG[ti];
                if(dr<1.0 && da<1.0){ double d=dr+da; if(d<bestd[i]){ bestd[i]=d; owner[i]=ti; } }
            }
        }
        if(tier){
            for(int i=0;i<m;i++){ tier1[i] = owner[i]>=0; if(!tier1[i]) bestd[i]=1e9; }
        }
    }
    /* ---- update tracks ---- */
    static double gr[RADAR_MAX_POINTS], ga[RADAR_MAX_POINTS], ge[RADAR_MAX_POINTS];
    for(int oi=0;oi<R->nord;oi++){
        int ti=R->ord[oi]; Track *t=&R->tracks[ti];
        int ng=0, nmv_hit=0, any_unk=0; double smax=-1e9;
        static double gv[RADAR_MAX_POINTS];
        for(int i=0;i<m;i++) if(owner[i]==ti){
            gr[ng]=rs[i]; ga[ng]=azs[i]; ge[ng]=els[i];
            if(ismv[i]){                       /* brightness: MOVING points only */
                if(snrok[i]){ if(snrs[i]>smax) smax=snrs[i]; } else any_unk=1;
                gv[nmv_hit++]=dops[i];
            }
            if(srcpt[i]>=0&&srcpt[i]<n) pts[srcpt[i]].tid=t->tid;
            ng++;
        }
        if(ng){
            double mr=med(gr,ng), maz=med(ga,ng), mel=med(ge,ng);
            double jd=hypot(mr-PR[ti], (maz-PAZ[ti])*DEG*mr);
            t->jit=(1-JIT_ALPHA)*t->jit + JIT_ALPHA*jd;
            t->mv_ewma=0.85*t->mv_ewma + 0.15*(double)nmv_hit;
            if(t->mv_ewma>t->max_mv) t->max_mv=t->mv_ewma;
            t->st_frames = (t->mv_ewma>=ST_MV_LO)?0:t->st_frames+1;
            t->r=ALPHA*mr+(1-ALPHA)*PR[ti];
            t->az=ALPHA_AZ*maz+(1-ALPHA_AZ)*PAZ[ti];
            t->el=(1-EL_ALPHA)*t->el + EL_ALPHA*mel;
            if(any_unk) t->snr_unknown=1;      /* no TLV7 -> fail open */
            /* flood brightness belongs to the flood, not to any track */
            if(smax>t->snr_peak
               && !(now_t < R->flood_until && t->r < FLOOD_R+FLOOD_MARGIN))
                t->snr_peak=smax;
            t->liar_evid = nmv_hit>0;          /* fresh claimed doppler this frame */
            double vd=0.0;
            if(nmv_hit){                       /* doppler self-consistency */
                vd=med(gv,nmv_hit);
                double derr=fabs(vd - t->vr);
                t->dop_err=(1-DOP_ALPHA)*t->dop_err + DOP_ALPHA*derr;
            }
            /* half-extents from the frame's points (for the wire box) */
            double mnx=1e9,mxx=-1e9,mny=1e9,mxy=-1e9,mnz=1e9,mxz=-1e9;
            for(int i=0;i<m;i++) if(owner[i]==ti){
                double rr=rs[i], a=azs[i]*DEG, e=els[i]*DEG, rh=rr*cos(e);
                double x=rh*sin(a), y=rh*cos(a), z=rr*sin(e);
                mnx=fmin(mnx,x); mxx=fmax(mxx,x);
                mny=fmin(mny,y); mxy=fmax(mxy,y);
                mnz=fmin(mnz,z); mxz=fmax(mxz,z);
            }
            t->sx=clampd((mxx-mnx)/2, MIN_SIZE_M, MAX_SIZE_M);
            t->sy=clampd((mxy-mny)/2, MIN_SIZE_M, MAX_SIZE_M);
            t->sz=clampd((mxz-mnz)/2, MIN_SIZE_M, MAX_SIZE_M);
            hist_push(t, now_t, t->r, t->az, nmv_hit>0 ? vd : (double)NAN);
            refit_vel(t, now_t);
            hit_push(t, 1); t->misses=0; t->age++; t->hits_total++;
        } else {
            double rad, cross;
            recent_motion(t, now_t, PARK_WIN, &rad, &cross);
            if(t->confirmed && t->moved_frames>=MOVE_CONFIRM
               && (now_t - t->last_moved_t) <= R->park_s
               && rad < PARK_DISP){
                t->vr=0; t->va=0;    /* genuinely-moved target that stopped: hold */
            } else { t->r=PR[ti]; t->az=PAZ[ti]; }
            hit_push(t, 0); t->misses++; t->age++;
            t->mv_ewma*=0.85; if(t->mv_ewma<ST_MV_LO) t->st_frames++;
            t->liar_evid=0;
        }
    }
    /* ---- motion test (range-scaled, NON-latching) ---- */
    for(int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        double rad, cross;
        recent_motion(t, now_t, DISP_WIN, &rad, &cross);
        double cross_floor = EMIT_DISP;
        { double f = (ANG_FLOOR_DEG*DEG)*t->r; if(f>cross_floor) cross_floor=f; }
        t->moving = (fabs(t->vr)>=EMIT_SPD
                     || fabs(t->va)>=ANG_RATE_MIN
                     || rad>=EMIT_DISP
                     || cross>=cross_floor);
        if(t->moving){ t->moved_frames++; t->last_moved_t=now_t; }
    }
    /* ---- post-confirmation consistency guard ---- */
    for(int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        if(!t->confirmed) continue;
        double r_path,r_net,r_span,c_path,c_net; int tele;
        int gn = guard_metrics(t, now_t, TELE_DM + TELE_RK*t->r,
                               &r_path,&r_net,&r_span,&c_path,&c_net,&tele);
        int judged = gn>=GUARD_MIN_N && r_span>1e-9;
        double c_move_floor = POS_NET;
        { double f=(ANG_FLOOR_DEG*DEG)*t->r; if(f>c_move_floor) c_move_floor=f; }
        int r_bad=0, spd_bad=0, tele_bad=0, c_bad=0;
        if(judged){
            double r_floor = R_PATH_MIN + R_PATH_RK*t->r;
            r_bad = (r_path>=r_floor && r_net/r_path<COH_MIN);
            spd_bad = (r_path/r_span > GSPD_MAX);
            tele_bad = (tele>=TELE_K);
            double c_floor = CROSS_WANDER_K*(ANG_FLOOR_DEG*DEG)*t->r;
            if(c_floor<r_floor) c_floor=r_floor;
            c_bad = (c_path>=c_floor && c_net/c_path<COH_MIN);
        }
        /* only judge jitter on frames WITH evidence: during a miss streak
         * the EWMA is stale and the window is starving - the miss-based
         * lifecycle owns that case, not the guard */
        int jit_bad = judged && t->jit > JIT_BASE + JIT_RK*t->r;
        /* coherent net progress per DOMAIN: a directed mover. A coherent
         * mover dragging its gate through clutter shows jitter and flutter
         * in the OTHER domain - measurement extent, not a ghost re-latch.
         * KILL only on overall coherence failure or the always-hard physics
         * tests; single-domain incoherence on a coherent mover only
         * UNLATCHES; jitter on a coherent mover is excused. */
        int prog_r = judged && r_path>0.0 && r_net>=POS_NET
                     && r_net/r_path>=COH_MIN;
        int prog_c = judged && c_path>0.0 && c_net>=c_move_floor
                     && c_net/c_path>=COH_MIN;
        int prog = prog_r || prog_c;
        int hard = spd_bad || tele_bad
                   || ((r_bad || c_bad || jit_bad) && !prog);
        int soft = !hard && ((r_bad && !prog_r) || (c_bad && !prog_c));
        /* gate freeze only on OVERALL incoherence (freezing a coherent
         * mover's gates while clutter disturbs one domain starves it into a
         * dropout death) */
        t->coh_bad = spd_bad || ((r_bad || c_bad) && !prog);
        if(hard) t->guard_bad++;
        else if(t->guard_bad>0) t->guard_bad--;
        if(soft) t->jit_ctr++;
        else if(t->jit_ctr>0) t->jit_ctr--;
        if(hard || soft) t->ok_streak=0;
        else {
            int frozen = (now_t < R->flood_until) && (t->r < FLOOD_R+FLOOD_MARGIN);
            int standing = t->ever_passed && fabs(t->r - t->pass_r) < RELATCH_DR;
            if(judged && !frozen
               && (r_net>=POS_NET || c_net>=c_move_floor || standing))
                t->ok_streak++;
        }
        int need = t->ever_passed ? GUARD_EMIT_RE : GUARD_EMIT;
        if(t->ok_streak>=need){ t->guard_pass=1; t->ever_passed=1; }
        if(t->ok_streak>=GUARD_EMIT) t->deep_pass=1;
        if(t->guard_pass) t->pass_r=t->r;
        if(t->guard_bad>=GUARD_UNLATCH || t->jit_ctr>=GUARD_UNLATCH)
            t->guard_pass=0;
    }
    /* ---- liar latch: starved doppler claims past LIAR_R_MIN (see LIAR_*) ---- */
    for(int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        if(!t->confirmed || !t->liar_evid) continue;   /* fresh doppler only */
        if(t->r<=LIAR_R_MIN) continue;                 /* near-field: not judged */
        double D, dR, tcov, c_net, c_path, meddop;
        int nw = walk_metrics(t, now_t, &D, &dR, &tcov, &c_net, &c_path, &meddop);
        if(nw<GUARD_MIN_N) continue;                   /* window too thin: hold */
        double span=0.0;
        {
            double ht[HMAX], hr[HMAX], ha[HMAX];
            int n = hist_window(t, now_t, WALK_WIN, ht, hr, ha);
            if(n>=2) span = ht[n-1]-ht[0];
        }
        if(span>=LIAR_SPAN_MIN && tcov<WALK_COV_MIN_S && meddop>=WALK_DOP_MIN)
            t->liar_bad++;                             /* starved claims */
        else if(tcov>=WALK_COV_MIN_S && t->liar_bad>0)
            t->liar_bad--;                             /* guard_bad hysteresis */
        if(t->liar_bad>=LIAR_KILL) t->liar=1;          /* latch: off the wire */
    }
    /* ---- lifecycle + confirmation (python order) ---- */
    for(int oi=0;oi<R->nord;){
        Track *t=&R->tracks[R->ord[oi]];
        if(!t->confirmed && t->misses>TENT_MAX_MISS){ ord_remove(R,oi); continue; }
        if(t->confirmed && t->guard_bad>=GUARD_KILL){ ord_remove(R,oi); continue; }
        int moved_recently = t->moved_frames>=MOVE_CONFIRM
                             && (now_t - t->last_moved_t) <= R->park_s;
        if(t->confirmed && t->misses>coast_frames){
            if(!(moved_recently && t->misses<=park_frames)){
                if(t->guard_pass && !t->liar && R->ngrave<GRAVE_MAX){
                    Grave *g=&R->grave[R->ngrave++];
                    g->t=now_t; g->r=t->r; g->az=t->az; g->snr_peak=t->snr_peak;
                }
                ord_remove(R,oi); continue;
            }
        }
        /* phantom cull: confirmed but never genuinely moved -> false-alarm cluster */
        if(t->confirmed && !t->moving && !moved_recently
           && (now_t - t->last_moved_t) > PHANTOM_LEASH_S){ ord_remove(R,oi); continue; }
        if(!t->confirmed){
            if(hit_sum_last(t,conf_n)>=conf_m && t->mv_ewma>=MV_RATE_MIN && t->jit<JIT_MAX)
                t->confirmed=1;
            else if(t->hitn>=ST_CONF_N && hit_sum_last(t,ST_CONF_N)>=ST_CONF_M && t->jit<ST_JIT_MAX)
                t->confirmed=1;
        }
        oi++;
    }
    /* purge graveyard */
    {
        int w=0;
        for(int i=0;i<R->ngrave;i++)
            if(now_t - R->grave[i].t <= INHERIT_S) R->grave[w++]=R->grave[i];
        R->ngrave=w;
    }
    /* ---- merge duplicates (keep the elder): stable sort by
     * (confirmed desc, age desc), then pairwise kill later entries ---- */
    for(int i=1;i<R->nord;i++){
        int key=R->ord[i]; Track *tk=&R->tracks[key];
        int j=i-1;
        while(j>=0){
            Track *tj=&R->tracks[R->ord[j]];
            int before = (tk->confirmed>tj->confirmed) ||
                         (tk->confirmed==tj->confirmed && tk->age>tj->age);
            if(before){ R->ord[j+1]=R->ord[j]; j--; } else break;
        }
        R->ord[j+1]=key;
    }
    for(int i=0;i<R->nord;i++){
        Track *a=&R->tracks[R->ord[i]];
        for(int j=i+1;j<R->nord;){
            Track *b=&R->tracks[R->ord[j]];
            double cross=fabs((a->az-b->az)*DEG)*0.5*(a->r+b->r);
            if(fabs(a->r-b->r)<MERGE_R && cross<MERGE_CROSS && fabs(a->vr-b->vr)<R->merge_dv)
                ord_remove(R,j);
            else j++;
        }
    }
    /* ---- seed from unclaimed MOVING points ---- */
    if(m){
        static double sr[RADAR_MAX_POINTS], sa[RADAR_MAX_POINTS], se[RADAR_MAX_POINTS], ss[RADAR_MAX_POINTS], sd[RADAR_MAX_POINTS];
        static int sk[RADAR_MAX_POINTS]; int ns=0;
        for(int i=0;i<m;i++) if(owner[i]<0 && ismv[i]){
            sr[ns]=rs[i]; sa[ns]=azs[i]; se[ns]=els[i]; ss[ns]=snrs[i]; sd[ns]=dops[i]; sk[ns]=snrok[i]; ns++;
        }
        if(ns){
            static int lbl[RADAR_MAX_POINTS]; int k=cluster_pts(sr,sa,ns,SEED_LINK_R,SEED_LINK_CROSS,lbl);
            for(int c=0;c<k;c++){
                double sumr=0,suma=0,smax=-1e9; int cnt=0, any_unk=0;
                static double eb[RADAR_MAX_POINTS], db[RADAR_MAX_POINTS]; int ne=0;
                for(int i=0;i<ns;i++) if(lbl[i]==c){
                    sumr+=sr[i]; suma+=sa[i]; db[ne]=sd[i]; eb[ne++]=se[i]; cnt++;
                    if(sk[i]){ if(ss[i]>smax) smax=ss[i]; } else any_unk=1;
                }
                if(cnt<R->min_pts) continue;
                double crr=sumr/cnt, caz=suma/cnt, cel=med(eb,ne), cdop=med(db,ne);
                int guarded=0;
                for(int oi=0;oi<R->nord;oi++){
                    Track *t=&R->tracks[R->ord[oi]];
                    if(fabs(t->r-crr)<SEED_GUARD_R && fabs(t->az-caz)<SEED_GUARD_AZ){ guarded=1; break; }
                }
                if(guarded) continue;
                int slot=-1;
                for(int ti=0;ti<MAX_TRK;ti++) if(!R->tracks[ti].used){ slot=ti; break; }
                if(slot<0) continue;
                Track *t=&R->tracks[slot]; memset(t,0,sizeof(*t));
                t->used=1; t->tid=R->next_tid++; t->r=crr; t->az=caz; t->el=cel;
                t->sx=t->sy=t->sz=MIN_SIZE_M;
                t->last_moved_t=now_t;
                t->pass_r=crr;
                if(any_unk) t->snr_unknown=1;
                if(smax>-1e9
                   && !(now_t < R->flood_until && crr < FLOOD_R+FLOOD_MARGIN))
                    t->snr_peak=smax;
                /* emission-evidence handoff from a just-died emitting track */
                for(int gi=0;gi<R->ngrave;gi++){
                    Grave *g=&R->grave[gi];
                    double cross=fabs((g->az-caz)*DEG)*0.5*(g->r+crr);
                    if(fabs(g->r-crr)<INHERIT_R && cross<INHERIT_CROSS){
                        t->guard_pass=1; t->ever_passed=1;
                        t->pass_r=crr;
                        if(g->snr_peak>t->snr_peak) t->snr_peak=g->snr_peak;
                        break;
                    }
                }
                hist_push(t,now_t,crr,caz,cdop); hit_push(t,1); t->age=1; t->hits_total=1;
                R->ord[R->nord++]=slot;
            }
        }
    }
    /* ---- occupancy update ---- */
    double lr = warm ? LEARN_FAST : LEARN;
    static char hitcell[NR][NA]; memset(hitcell,0,sizeof(hitcell));
    for(int i=0;i<nall;i++){ int ir=(int)((ar[i]-GR_R0)/GR_DR), ia=(int)((aa_[i]-GR_A0)/GR_DA);
        if(ir>=0&&ir<NR&&ia>=0&&ia<NA) hitcell[ir][ia]=1; }
    for(int a=0;a<NR;a++) for(int b=0;b<NA;b++){ R->occ[a][b]*=(float)(1.0-DECAY); if(hitcell[a][b]) R->occ[a][b]=(float)(R->occ[a][b]*(1-lr)+lr); }

    /* ---- emit: coherent, bright, genuinely-moving confirmed tracks only ---- */
    static int ci[MAX_TRK]; int ncand=0;
    for(int oi=0;oi<R->nord;oi++){
        int ti=R->ord[oi]; Track *t=&R->tracks[ti];
        if(!t->confirmed) continue;
        if(!t->guard_pass) continue;            /* no positive coherence yet */
        if(t->liar) continue;                   /* latched ghost: never emitted */
        {
            double req = snr_req(t->r);
            double ramp = (t->r - FAR_MARGIN_R0) / (FAINT_R - FAR_MARGIN_R0);
            req += FAR_MARGIN_DB * clampd(ramp, 0.0, 1.0);
            if(!t->snr_unknown && t->snr_peak < req
               /* faint-far relief: FULL coherent streak + doppler-consistent */
               && !(t->r >= FAINT_R && t->deep_pass && t->dop_err <= DOP_GATE))
                continue;                       /* not target-bright for THIS range */
        }
        int moved_recently = t->moved_frames>=MOVE_CONFIRM
                             && (now_t - t->last_moved_t) <= R->park_s;
        if(!(t->moving || moved_recently)) continue;   /* static/phantom */
        if(t->misses>EMIT_MAX_MISS && !moved_recently) continue;
        if(fabs(t->az)>R->fov_half || t->r>OUT_R_MAX) continue;
        ci[ncand++]=ti;
    }
    /* stable sort by strength: mv_ewma + 0.001*min(age,500), descending */
    for(int i=1;i<ncand;i++){
        int key=ci[i]; Track *tk=&R->tracks[key];
        double ks=tk->mv_ewma+0.001*(tk->age<500?tk->age:500); int j=i-1;
        while(j>=0){ Track *tj=&R->tracks[ci[j]]; double js=tj->mv_ewma+0.001*(tj->age<500?tj->age:500);
            if(js<ks){ci[j+1]=ci[j];j--;}else break; } ci[j+1]=key;
    }
    int nout=0;
    static Track *etrk[RADAR_MAX_TARGETS];        /* tracks behind out[] */
    for(int i=0;i<ncand && nout<max_out;i++){
        Track *t=&R->tracks[ci[i]]; int dup=0;
        /* spatial dedup against already-emitted (recover r,az from x,y) */
        for(int e=0;e<nout;e++){
            double ex=out[e].x, ey=out[e].y, er=hypot(ex,ey), eazd=atan2(ex,ey)/DEG;
            double cross=fabs((eazd-t->az)*DEG)*0.5*(er+t->r);
            if(cross<R->dedup_cross && fabs(er-t->r)<DEDUP_R){ dup=1; break; }
        }
        if(dup) continue;
        /* reflection-copy suppressor (see REFL_* block): the shadow clock
         * is maintained below for every confirmed track; here it decides */
        if(t->shadow_s >= REFL_SHADOW_S) continue;      /* convicted copy */
        int suspect = t->shadow_s > 0.0;        /* on watch: emit, flagged */
        double rr=t->r, a=t->az*DEG, e=t->el*DEG, rh=rr*cos(e);
        double vrad=t->vr, vaz=t->va*DEG;
        RadarTarget *o=&out[nout++];
        o->tid=t->tid;
        o->x=(float)(rh*sin(a)); o->y=(float)(rh*cos(a)); o->z=(float)(rr*sin(e));
        o->vx=(float)(vrad*sin(a)+rr*cos(a)*vaz);
        o->vy=(float)(vrad*cos(a)-rr*sin(a)*vaz);
        o->vz=0.0f;
        o->sx=(float)t->sx; o->sy=(float)t->sy; o->sz=(float)t->sz;
        double conf=t->hits_total/10.0; if(conf>1.0)conf=1.0;
        o->conf=(float)conf; o->num_points=t->hits_total;
        o->suspect=suspect;
        etrk[nout-1]=t;
    }
    /* ---- reflection-shadow bookkeeping: every confirmed track vs the
     * frame's emitted set (see REFL_* block). Runs after emission so a
     * copy serves its sentence while still earning its first emit. ---- */
    for(int oi=0;oi<R->nord;oi++){
        Track *t=&R->tracks[R->ord[oi]];
        if(!t->confirmed) continue;
        double tstr = t->mv_ewma + 0.001*(t->age<500?t->age:500);
        int shadowed=0; Track *src=NULL;
        for(int e=0;e<nout;e++){
            Track *s=etrk[e];
            if(s==t) continue;
            double sstr = s->mv_ewma + 0.001*(s->age<500?s->age:500);
            if(sstr <= tstr) continue;          /* only a STRONGER source */
            if(fabs(s->r - t->r) >= REFL_R) continue;
            double dv=1e18;                     /* signed, wrap-aware */
            for(int k=-2;k<=2;k++){
                double d=fabs(s->vr - t->vr + 2.0*k*V_FOLD_MPS);
                if(d<dv) dv=d;
            }
            if(dv >= REFL_DV) continue;
            double sep=fabs((s->az - t->az)*DEG)*0.5*(s->r + t->r);
            double sep_min=REFL_SEP_DEG*DEG*0.5*(s->r + t->r);
            if(sep_min < REFL_SEP_M) sep_min = REFL_SEP_M;
            if(sep > sep_min){ shadowed=1; src=s; break; }
        }
        if(shadowed){
            t->shadow_s += dt; t->clear_s = 0.0;
            /* proven mirror-breeder source: its later copies convict on
             * sight (each ghost episode is shorter than the dwell, but the
             * SOURCE geometry already served full time) */
            if(src->breeder && t->shadow_s < REFL_SHADOW_S)
                t->shadow_s = REFL_SHADOW_S;
            if(t->shadow_s >= REFL_SHADOW_S) src->breeder = 1;
        } else if(t->shadow_s > 0.0){
            t->clear_s += dt;                   /* decisive separation only */
            if(t->clear_s >= REFL_CLEAR_S){ t->shadow_s = 0.0; t->clear_s = 0.0; }
        }
    }
    return nout;
}

#ifdef CLUSTER_INTROSPECT
/* Bench-tool hook (radar/tools/track_replay.c): confirmed tracks in list
 * order, for parity diffing against the Python reference. Not compiled into
 * the daemon. */
int cluster_confirmed(const RadarClusterer *R, int *tid, double *r, double *az, int max)
{
    int k=0;
    for(int oi=0;oi<R->nord && k<max;oi++){
        const Track *t=&R->tracks[R->ord[oi]];
        if(t->confirmed){ tid[k]=t->tid; r[k]=t->r; az[k]=t->az; k++; }
    }
    return k;
}
int cluster_all(const RadarClusterer *R, int *tid, double *r, double *az, int *conf, int max)
{
    int k=0;
    for(int oi=0;oi<R->nord && k<max;oi++){
        const Track *t=&R->tracks[R->ord[oi]];
        tid[k]=t->tid; r[k]=t->r; az[k]=t->az; conf[k]=t->confirmed; k++;
    }
    return k;
}
int cluster_track_detail(const RadarClusterer *R, int want_tid, double *out7)
{
    for(int oi=0;oi<R->nord;oi++){
        const Track *t=&R->tracks[R->ord[oi]];
        if(t->tid==want_tid){
            out7[0]=t->r; out7[1]=t->az; out7[2]=t->snr_peak;
            out7[3]=t->guard_pass; out7[4]=t->ever_passed;
            out7[5]=t->ok_streak; out7[6]=t->guard_bad;
            return 1;
        }
    }
    return 0;
}
#endif
