/* config.h - fusion module geometry, pools, knob defaults/limits, constants.
 *
 * fusiond joins the radar tracker wire (:8092) and the EO tracker wire (:8095)
 * into one target picture on :8096. All angles are radians internally and on
 * the wire (the eotrack convention: az +right, el +up, rig frame = the EO
 * sensor frame; radar-sourced angles get the mount trim added).
 */
#ifndef FUS_CONFIG_H
#define FUS_CONFIG_H

#define FUS_VERSION "0.1.0"

/* ---- ports / peers ---- */
#define FUS_PORT_DEFAULT   8096
#define RAD_STREAM_HOST    "127.0.0.1"
#define RAD_STREAM_PORT    8092
#define TRK_STREAM_HOST    "127.0.0.1"
#define TRK_STREAM_PORT    8095

/* ---- pools (static, no malloc in the hot path) ---- */
#define FUS_MAX_RAD    64     /* radar targets mirrored per frame */
#define FUS_MAX_EO     64     /* EO tracks mirrored per frame */
#define FUS_MAX_TRK    128    /* fusion tracks (gid rows), covers both sides unfused */
#define FUS_MAX_CAND   64     /* candidate pairs being confirmed */
#define FUS_MAX_OUT    128    /* emitted rows per frame */

/* ---- association sigmas (radians / SI). TUNE on recordings - see README. ---- */
#define FUS_SIG_RAD_AZ     0.0105   /* radar azimuth 1-sigma, 0.6 deg */
#define FUS_SIG_RAD_EL     0.0698   /* radar elevation 1-sigma, 4.0 deg: deliberately
                                     * huge - elevation only separates gross cases
                                     * (2-row interferometer physics floor + bias +
                                     * conditioning lag). */
#define FUS_SIG_RAD_RDOT   0.5      /* radar radial-rate 1-sigma, m/s */
#define FUS_SIG_EO_RATE    0.003    /* EO angular-rate 1-sigma, rad/s */
#define FUS_SIG_GROW       0.05     /* looming-consistency 1-sigma, 1/s */
#define FUS_EO_SIG_FLOOR   0.0005   /* floor on EO s_ang (rad) so a razor-sharp
                                     * track cannot shrink the gate to nothing */

/* Drop the elevation term entirely when either side shows fast vertical motion
 * (climbing-drone rule: radar's lagged conditioned el would veto a true match). */
#define FUS_EL_RATE_DROP   0.07     /* rad/s ~= 4 deg/s */

/* Grow cross-check armed only when the radar sees a verified mover genuinely
 * closing, and the EO track is old enough for its grow EMA to mean something. */
#define FUS_GROW_ARM_GPRED 0.03     /* 1/s */
#define FUS_GROW_ARM_AGE_S 1.0

/* chi-square 99% gates by degrees of freedom used (2..5 terms) */
#define FUS_CHI2_2  9.21
#define FUS_CHI2_3  11.34
#define FUS_CHI2_4  13.28
#define FUS_CHI2_5  15.09
#define FUS_GATE_INCUMBENT 1.5      /* existing pair keeps matching at gate x1.5 */
#define FUS_STICKY_BONUS   3.0      /* subtracted from an incumbent's D^2 in assignment */

/* Physical-size consistency: the pair hypothesis carries the radar's range, so
 * the EO box converts to metres and must roughly agree with the radar's box.
 * Log-ratio cost: a 2x mismatch is one sigma, a 4-5x mismatch (walker vs
 * parked car) is a strong veto. Radar boxes are rough - keep it soft. TUNE. */
#define FUS_SIG_LNSIZE  0.69       /* one sigma = 2x size ratio */
#define FUS_SIZE_MIN_M  0.3        /* floor on either width, m */
/* The size test is ONE-SIDED: only "camera object bigger than radar object"
 * counts against a pair. Radar near-field boxes smear fat (a person at 30 m
 * came back 5-7 m wide), so radar-bigger-than-camera proves nothing. */

/* A camera track that has PROVABLY not moved for seconds is parked scenery -
 * a moving radar target may not marry it. Scoped to close/mid range: a far
 * radial walker genuinely looks angle-static to the camera and must not be
 * vetoed (radar owns radial at range; drift-divorce covers wrong pairs there). */
#define FUS_PARKED_VETO_RMAX   200.0   /* apply below this radar range, m */
#define FUS_PARKED_VETO_SPEED  3.0     /* radar speed above this, m/s */
#define FUS_PARKED_MIN_AGE_S   3.0     /* EO history needed to call it parked */
#define FUS_PARKED_AZ_RAD      0.0017  /* < 0.1 deg net drift ... */
#define FUS_PARKED_LNSZ        0.03    /* ... and < 3% net size change */

/* courtship tolerates brief EO coasting (close range coasts a lot) */
#define FUS_EO_COURT_COAST_S   0.5
/* married pairs hold through more jitter than candidates (stickiness) */
#define FUS_GATE_MARRIED       2.0

/* Passing signature: a same-object pair's angle residual is stationary noise;
 * a radar target sliding past a parked object shows a monotonic residual
 * trend. Net drift beyond this across the trend window blocks confirmation
 * and fast-divorces a fused pair. TUNE on crossing/passing recordings. */
#define FUS_TREND_WIN   8          /* co-fresh evals in the trend window */
#define FUS_TREND_NET   0.0105     /* rad (0.6 deg) net drift across the window */

/* A marriage needs time as well as hits: a driving car sweeping along a row
 * of parked cars confirms 4-of-6 in ~0.3 s, before its drift is visible.
 * Requiring this much observation span lets the drift show first. TUNE. */
#define FUS_CONFIRM_SPAN_S 0.7

/* Slow drifters: a wrong pair drifting at ~0.2 deg/s never trips the short
 * trend window. Every married pair remembers the residual it married at;
 * moving this far from it (mean of the ring vs the reference) is definitive
 * evidence of a wrong marriage - divorce immediately. TUNE. */
#define FUS_DRIFT_ABS   0.014      /* rad (0.8 deg) from the marriage reference */

/* After a drift divorce, the radar tid may not marry ANYONE for this long -
 * a sweeping radar target otherwise chains through every parked car in turn. */
#define FUS_RAD_COOLDOWN_S 1.5

/* Trim estimator sampling: only ISOLATED geometry teaches the trim - a radar
 * verified mover with exactly one confirmed EO candidate within the window.
 * (Sampling matched pairs only was selection-biased when the trim was off.) */
#define FUS_EST_ISO_RAD 0.035      /* rad (~2 deg): isolation window */

/* ---- pair lifecycle (co-fresh evaluations; time knobs are /ctl-settable) ---- */
#define FUS_CONFIRM_DEFAULT 3       /* /ctl confirm: promote at confirm+1 of 2*confirm */
#define FUS_CONFIRM_MIN     1
#define FUS_CONFIRM_MAX     8
#define FUS_DIVORCE_S_DEFAULT 0.6   /* sustained co-fresh disagreement before split */
#define FUS_DIVORCE_S_MIN     0.2
#define FUS_DIVORCE_S_MAX     3.0
#define FUS_REPAIR_BAR_S      1.0   /* divorced tids may not re-pair for this long */

/* ---- freshness / staleness ---- */
#define FUS_RAD_FRESH_S    0.30     /* radar frame older than this = stale side */
#define FUS_EO_FRESH_S     0.50     /* covers operator-set detector cadence down to ~4 fps */
#define FUS_EO_COFRESH_S   0.20     /* EO coast_s above this doesn't count as co-fresh */
#define FUS_MAX_EXTRAP_S   0.15     /* never predict a track further than this */
#define FUS_EXTRAP_INFL    0.5      /* sigma inflation: s^2 += (INFL * rate * dt)^2 */

/* ---- fused state composition ---- */
#define FUS_EO_ANGLE_MAX_COAST 1.0  /* EO owns fused angles while coast_s < this */
#define FUS_R_PROP_S       1.0      /* radar lost: propagate range this long ... */
#define FUS_R_STALE_S      1.0      /* ... flag r_stale beyond this ... */
#define FUS_R_DROP_S       3.0      /* ... drop range entirely beyond this */
#define FUS_RDOT_EMA       0.3      /* light EMA on radial rate */
#define FUS_R_CLAMP_BASE   5.0      /* innovation clamp: max(BASE, 3*bin + |rdot|*dt) */
#define FUS_R_CLAMP_BIN    2.6      /* radar range bin, m */
#define FUS_CLS_DECAY_TAU  5.0      /* class vote memory, seconds */
#define FUS_CLS_SWITCH     1.5      /* challenger must beat incumbent votes by this */
#define FUS_MIN_RAD_CONF   0.3      /* radar conf floor to start a new pair */

/* ---- global id lifecycle ---- */
#define FUS_REBIND_SIGMA_K 2.0      /* re-bind gate = K x association sigmas */
#define FUS_REBIND_RANGE_M 15.0     /* + range window for radar re-bind */
#define FUS_REBIND_FRAMES  3        /* consecutive frames inside the gate to re-bind */
#define FUS_REBIND_GRACE_S 2.0      /* unbound gid survives this long awaiting re-bind */
#define FUS_RESET_GRACE_S  3.0      /* sensor-restart grace (sigmas x2 while it lasts) */

/* ---- EO field of view (fallback only - live values come off the EO wire) ---- */
#define EO_IMG_W_DEFAULT   1440
#define EO_IMG_H_DEFAULT   1088
#define EO_IFOV_RAD_DEFAULT 287.5e-6
#define FUS_FOV_MARGIN_RAD 0.0087   /* 0.5 deg margin inside the EO FOV edge */

/* ---- mount trim (degrees on the knob surface, radians internally) ---- */
#define FUS_TRIM_AZ_DEG_DEFAULT 1.1   /* GUI-measured overlay values, Asaf-approved seed */
#define FUS_TRIM_EL_DEG_DEFAULT 2.2
#define FUS_TRIM_ABS_MAX_DEG    10.0
#define FUS_TRIM_FILE "/var/lib/airpoc/fusion-trim.json"
#define FUS_TRIM_FILE_FALLBACK "./fusion-trim.json"
#define FUS_TRIM_EST_RING 512       /* observe-only residual estimator ring */

/* ---- /ctl gate scale ---- */
#define FUS_GATE_SCALE_DEFAULT 1.0
#define FUS_GATE_SCALE_MIN     0.25
#define FUS_GATE_SCALE_MAX     4.0
#define FUS_COAST_S_DEFAULT    1.0  /* /ctl coast_s: lost-constituent contribution window */
#define FUS_COAST_S_MIN        0.2
#define FUS_COAST_S_MAX        5.0

/* ---- recorder tap ---- */
#define FUS_TAP_NAME  "airpoc.fus_wire"
#define FUS_TAP_SLOTS 16
#define FUS_TAP_BYTES (128 * 1024)

#endif
