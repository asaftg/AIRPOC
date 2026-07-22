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

/* chi-square 99% gates by degrees of freedom used (2/3/4 terms) */
#define FUS_CHI2_2  9.21
#define FUS_CHI2_3  11.34
#define FUS_CHI2_4  13.28
#define FUS_GATE_INCUMBENT 1.5      /* existing pair keeps matching at gate x1.5 */
#define FUS_STICKY_BONUS   3.0      /* subtracted from an incumbent's D^2 in assignment */

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
