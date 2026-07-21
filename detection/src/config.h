/* config.h — compile-time geometry and runtime knob bounds for detectiond.
 *
 * The EO camera geometry is fixed by the sensor; the IFOV depends on the lens
 * and is a RUNTIME value (--ifov / --focal) because the lens has been changed
 * on the bench and the installed focal length must be confirmed there. The
 * default below is the documented CommonLands CIL122 (f=12 mm) on the IMX296
 * (3.45 µm pixel): IFOV = 3.45e-6 / 12e-3 = 287.5 µrad/px.
 */
#ifndef DET_CONFIG_H
#define DET_CONFIG_H

#define DET_VERSION       "0.6.0"          /* track-before-detect temporal integration; motion worker frozen */

/* IMX296 native frame delivered on airpoc.eo_y10 (Y10 in 16-bit LE words). */
#define EO_IMG_W          1440
#define EO_IMG_H          1088
#define EO_BYTES_PER_PX   2
#define EO_FRAME_BYTES    ((size_t)EO_IMG_W * EO_IMG_H * EO_BYTES_PER_PX)   /* 3,133,440 */
#define EO_TAP_NAME       "airpoc.eo_y10"

/* Optical defaults (overridable at runtime — see above). */
#define EO_PIXEL_UM_DEFAULT   3.45
#define EO_FOCAL_MM_DEFAULT   12.0
/* IFOV in radians/pixel = pixel_um*1e-6 / (focal_mm*1e-3). */
#define EO_IFOV_RAD_DEFAULT   (EO_PIXEL_UM_DEFAULT * 1e-6 / (EO_FOCAL_MM_DEFAULT * 1e-3))

/* Image-centre pixel coordinates for the pixel->angle mapping. */
#define EO_CX             ((EO_IMG_W - 1) / 2.0)   /* 719.5 */
#define EO_CY             ((EO_IMG_H - 1) / 2.0)   /* 543.5 */

/* Detector knob defaults + clamp bounds (echoed in /stats, set via /ctl). The
 * cadence default runs the GPU detector at every 4th frame (~15/s when capture is
 * 60/s); it is meant to be raised toward 1 (every frame) as a target closes — a fast
 * crosser up close needs the higher rate. */
#define DET_CONF_DEFAULT       0.5     /* the "strong" tier: a detection at or above this
                                          is emitted immediately on its own, with no
                                          temporal integration and no added latency */
#define DET_CONF_MIN           0.05
#define DET_CONF_MAX           0.95
#define DET_CADENCE_DEFAULT    4       /* run detector every Nth captured frame */
#define DET_CADENCE_MIN        1
#define DET_CADENCE_MAX        8
#define DET_NMS_DEFAULT        0.45    /* box-merge IoU: lower = merge more aggressively
                                          (collapses the multiple boxes a big/close object
                                          produces). Also merges boxes mostly-contained in
                                          a higher-scoring one (see infer.cpp). */
#define DET_NMS_MIN            0.10
#define DET_NMS_MAX            0.90
#define DET_MAXDETS_DEFAULT    128
#define DET_MAXDETS_MIN        1
#define DET_MAXDETS_MAX        512

/* --- Temporal integration: track-before-detect (temporal.h) ---------------------
 * The per-frame threshold is a lossy gate — evidence below it is destroyed and no
 * downstream tracker can recover it. With this enabled the model is run at the low
 * floor `tbd_lo` and the weak candidates are integrated along a short trajectory;
 * one that accumulates enough evidence is promoted to a real detection (flagged
 * "tbd":1 on the wire). Detections already at/above `conf` are unaffected: same box,
 * same confidence, same tick, no added latency. The cost is latency ON WEAK TARGETS
 * ONLY — roughly (confirm / per-hit score) ticks, ~0.2-0.5 s at the default cadence. */
#define DET_TEMPORAL_DEFAULT   1       /* operator-facing on/off ("EO temporal" button) */

#define DET_TBD_LO_DEFAULT     0.15    /* candidate floor: the model is run at this when
                                          temporal is on, and it doubles as the evidence
                                          reference — a hit exactly here is judged as
                                          likely clutter as target and scores only the
                                          small fixed presence term */
#define DET_TBD_LO_MIN         0.02
#define DET_TBD_LO_MAX         0.50
#define DET_TBD_CONFIRM_DEFAULT 3.0    /* score needed to promote a weak track. This is a
                                          LATENCY knob, not a sensitivity one: a target the
                                          model keeps seeing crosses any threshold sooner or
                                          later, so raising it delays promotion rather than
                                          preventing it (measured: 2.0 -> 8.0 changed the box
                                          count by only 13% on a 30 s day clip). To accept
                                          FEWER things, raise `tbd_lo` instead. */
#define DET_TBD_CONFIRM_MIN    0.5
#define DET_TBD_CONFIRM_MAX    10.0    /* < DET_TBD_S_MAX so promotion stays reachable */
#define DET_TBD_DECAY_DEFAULT  0.7     /* score subtracted per tick with no observation.
                                          This is what kills flicker: a candidate seen
                                          every third tick nets negative and dies */
#define DET_TBD_DECAY_MIN      0.1
#define DET_TBD_DECAY_MAX      3.0
#define DET_TBD_MAXMISS_DEFAULT 3      /* consecutive missed ticks before a track is dropped */
#define DET_TBD_MAXMISS_MIN    1
#define DET_TBD_MAXMISS_MAX    10

/* Fixed internals (not knobs — changing these changes the algorithm, not the tuning). */
#define DET_TBD_MAX_TRACKS     256     /* candidate tracks carried at once */
#define DET_TBD_MAX_IN         512     /* candidates accepted per tick (== MAX_DETS_CAP) */
#define DET_TBD_PRESENCE       0.5     /* score for a hit at exactly `tbd_lo`: the bare
                                          fact that the model fired again, in the same
                                          place, along a plausible path */
#define DET_TBD_S_MAX          12.0    /* score ceiling — a long-lived strong track must
                                          still be able to die in bounded time */
#define DET_TBD_S_FLOOR       (-3.0)   /* score at which a track is abandoned */
#define DET_TBD_GATE_BASE_PX   24.0f   /* association gate = (this + box size) * scale */
#define DET_TBD_GATE_REF_FPS   15.0    /* gate scale reference: gate grows as ticks slow */

/* --- Motion worker: FROZEN (2026-07-21) ----------------------------------------
 * The motion head is retained but NOT under development, and is OFF by default.
 * It was measured across four recordings and does not do its job: it absorbs slow
 * or near-stationary targets (background-subtraction method), misses slow far
 * targets when the frame-difference baseline is short, and floods on wind-blown
 * foliage under both methods (128-353 surviving boxes on the day scene) — clutter
 * that no per-frame threshold can remove, because any threshold that kills foliage
 * also kills far targets. Temporal integration above supersedes it as the route to
 * weak/far targets.
 *
 * It is FROZEN rather than deleted for one reason: TBD recovers targets the model
 * scores WEAKLY, but cannot recover a target the model scores at ZERO (e.g. a 3 px
 * drone it has no notion of). Motion is the only path that ever sees those. It stays
 * available via /ctl (motion=1) so it can be revived if the trained model shows that
 * gap, and its knobs stay live for that evaluation. Do not tune it further, do not
 * surface it in the operator GUI, and do not build on it without new evidence. */
#define DET_MOTION_DEFAULT     0       /* FROZEN — off; see the note above */
#define DET_MOT_K_DEFAULT      6.0     /* MAD multiplier for the motion threshold */
#define DET_MOT_K_MIN          1.0
#define DET_MOT_K_MAX          30.0
#define DET_MOT_WINDOW_S_DEFAULT 15.0  /* rolling-background window (seconds) */
#define DET_MOT_WINDOW_S_MIN   1.0
#define DET_MOT_WINDOW_S_MAX   60.0
#define DET_MOT_PERSIST_DEFAULT 3      /* confirmation strength 1..5 = fraction of the ~1 s
                                          M-of-N tracker window that must hit */
#define DET_MOT_PERSIST_MIN    1
#define DET_MOT_PERSIST_MAX    5
#define DET_MOT_DOWN_DEFAULT   1       /* 1 = native; higher blinds small targets */
#define DET_MOT_DOWN_MIN       1
#define DET_MOT_DOWN_MAX       4
#define DET_MOT_METHOD_DEFAULT 1       /* 0 = background-subtraction, 1 = frame-difference */
#define DET_MOT_METHOD_MIN     0
#define DET_MOT_METHOD_MAX     1
#define DET_MOT_BASELINE_S_DEFAULT 2.0 /* frame-diff baseline, seconds back */
#define DET_MOT_BASELINE_S_MIN 0.25
#define DET_MOT_BASELINE_S_MAX 5.0
/* The motion worker runs on the SAME `cadence` tick as the appearance model. */

/* --- EO tap meta[] layout (published by eo/pipeline/libeo.c) --- */
/* meta[0]=v4l2_seq  meta[1]=exp_lines  meta[2]=gain  meta[3]=vmax
 * meta[4]=illum_state  meta[5]=drops_cum
 * illum_state = on | present<<1 | power<<8 | fov10<<16 */
#define EO_META_V4L2SEQ    0
#define EO_META_EXPLINES   1
#define EO_META_GAIN       2
#define EO_META_VMAX       3
#define EO_META_ILLUM      4
#define EO_META_DROPS      5

#define EO_ILLUM_ON(x)       ((x) & 0x1u)
#define EO_ILLUM_PRESENT(x)  (((x) >> 1) & 0x1u)
#define EO_ILLUM_POWER(x)    (((x) >> 8) & 0xFFu)
#define EO_ILLUM_FOV10(x)    (((x) >> 16) & 0xFFFFu)

#endif /* DET_CONFIG_H */
