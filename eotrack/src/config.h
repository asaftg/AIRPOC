/* config.h - geometry and runtime knob bounds for trackerd (the EO tracker).
 *
 * The EO tracker consumes the detector's per-frame boxes (:8094) and turns them
 * into persistent, smoothed, coasted tracks with stable IDs. It works in PIXELS
 * internally (that is what the detector reports and what the lock loop needs) and
 * converts to angles only when it emits, using the same IFOV the detector uses.
 *
 * The camera geometry is fixed by the sensor; the IFOV depends on the lens and is
 * a RUNTIME value (--ifov / --focal) - the default is the documented CommonLands
 * CIL122 (f=12 mm) on the IMX296 (3.45 um pixel): 287.5 urad/px.
 */
#ifndef TRK_CONFIG_H
#define TRK_CONFIG_H

#define TRK_VERSION       "0.1.0"

/* EO frame geometry - identical to the detector's config.h (the boxes are in this
 * frame). Y10 delivered as 16-bit LE words on airpoc.eo_y10. */
#define EO_IMG_W          1440
#define EO_IMG_H          1088
#define EO_BYTES_PER_PX   2
#define EO_FRAME_BYTES    ((size_t)EO_IMG_W * EO_IMG_H * EO_BYTES_PER_PX)
#define EO_TAP_NAME       "airpoc.eo_y10"

#define EO_PIXEL_UM_DEFAULT   3.45
#define EO_FOCAL_MM_DEFAULT   12.0
#define EO_IFOV_RAD_DEFAULT   (EO_PIXEL_UM_DEFAULT * 1e-6 / (EO_FOCAL_MM_DEFAULT * 1e-3))

/* Image-centre pixel coordinates for the pixel->angle mapping (az +right, el +up). */
#define EO_CX             ((EO_IMG_W - 1) / 2.0)   /* 719.5 */
#define EO_CY             ((EO_IMG_H - 1) / 2.0)   /* 543.5 */

/* EO tap meta[] layout (published by eo/pipeline/libeo.c; mirrored from the
 * detector's config.h so the lock loop can read exposure/gain/illuminator per
 * frame and freeze its template across an AE/illuminator step). */
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

/* --- Ports / names --- */
#define TRK_PORT_DEFAULT   8095
#define TRK_TAP_NAME       "airpoc.trk_wire"      /* recorder tap (byte-verbatim /stream) */
#define TRK_TAP_SLOTS      16
#define TRK_TAP_BYTES      (128 * 1024)
#define DET_STREAM_HOST    "127.0.0.1"
#define DET_STREAM_PORT    8094                   /* detector SSE we consume */

/* --- Fixed internals (algorithm, not tuning) --- */
#define TRK_MAX_TRACKS     128     /* live tracks carried at once (fixed pool) */
#define TRK_MAX_IN         512     /* detections accepted per tick (== detector MAXDETS) */
#define TRK_HIST           64      /* per-track position-history ring (for clutter test) */

/* --- Knob defaults + clamp bounds (echoed in /stats, set via /ctl) --- */

/* Association gate: a detection matches a track if within (base + 0.5*boxdim) px,
 * scaled by (ref_fps / measured_fps) so the same real speed tracks at any rate.
 * MEASURED tick rate, not configured (audit lesson). */
#define TRK_GATE_BASE_DEFAULT   20.0
#define TRK_GATE_BASE_MIN       4.0
#define TRK_GATE_BASE_MAX       200.0
#define TRK_GATE_REF_FPS        15.0
#define TRK_GATE_SIGMA_K        3.0     /* gate grows with the track's own uncertainty */
#define TRK_GATE_MAX_PX         120.0   /* cap so a huge box can't gate the whole scene */

/* Confirmation: a track emits once its evidence score crosses `confirm`. Model
 * detections add ~1.0 (a strong direct det) down to a floor for weak/tbd boxes;
 * a tbd-promoted box arrives already integrated upstream and counts fully. This
 * is TRACK HYGIENE (stop one-tick junk getting an ID), NOT a second sensitivity
 * layer - TBD confidence-raising lives in the detector. */
#define TRK_CONFIRM_DEFAULT     3.0
#define TRK_CONFIRM_MIN         1.0
#define TRK_CONFIRM_MAX         12.0
#define TRK_SCORE_MISS          1.0     /* score removed per missed tick */
#define TRK_SCORE_MAX           12.0    /* ceiling so a stale track still dies in bounded time */
#define TRK_SCORE_FLOOR        (-3.0)   /* score at which a tentative track is abandoned */

/* Coast: a confirmed track survives this many seconds of misses on held rates
 * before it dies. Derived to frames from the MEASURED tick rate at runtime. */
#define TRK_COAST_S_DEFAULT     1.0
#define TRK_COAST_S_MIN         0.2
#define TRK_COAST_S_MAX         5.0

/* Park-hold: a confirmed track that is essentially STATIONARY (a parked car, a
 * standing person) keeps its id far longer through a detection blink, instead of
 * dying on the 1 s coast and being re-numbered when the weakly-detected object
 * reappears (the "same car, new id every second" churn). Its box does not drift
 * because it is not moving, so a longer hold costs nothing while it is there;
 * a target that actually leaves holds stale for at most park_s then dies. A MOVING
 * lost target still uses the short coast (it is genuinely gone, don't chase it). */
#define TRK_PARK_S              4.0     /* stationary-confirmed coast budget (s) */
#define TRK_STATIONARY_VEL     10.0     /* |output velocity| below this = parked (px/s) */

/* Lock give-up: an operator-engaged track is held through a BRIEF loss (occlusion, a
 * frame the detector missed, a fast pan) so the lock feels solid - but if it goes
 * unsupported (no detection AND the 60 fps lock cannot find it) for this long, the
 * target has really left the field of view. The track is then dropped and the daemon
 * releases the lock (engaged -> -1) so the operator is never stuck locked on empty space.
 * Longer than the normal 1 s coast (so the lock is sticky through occlusion) but bounded.
 * A target that is really there is re-detected every tick or two, so it never approaches
 * this; only a target the detector has stopped seeing (left the FOV) does. */
#define TRK_LOCK_LOST_S         1.5

/* Clutter (translate-vs-oscillate) horizon: over this many seconds a track's net
 * displacement is compared to its path length. An oscillator (foliage) has a long
 * path and ~zero net -> emission-latched OFF. NO size/speed/displacement KILL gate
 * (standing rule: that deletes far/small/slow targets). Radial approachers net ~0
 * too and are saved by looming (size growth) or a classified model hit. */
#define TRK_CLUTTER_S_DEFAULT   2.0
#define TRK_CLUTTER_S_MIN       0.5
#define TRK_CLUTTER_S_MAX       6.0
#define TRK_TRANS_RATIO         0.30    /* net/path below this = oscillator (latch off) */
#define TRK_LOOM_RATE           0.15    /* rel size growth (1/s) that rescues a radial mover */

/* Cross-track dedup at emit: drop a track whose box is more than this fraction contained
 * in a stronger same-class track's box. The detector fragments a big/close object into
 * several overlapping boxes; this keeps one box per target (the strongest). The engaged
 * track is never dropped. */
#define TRK_DEDUP_CONTAIN       0.55

/* Output smoothing = a FIRM position low-pass (EMA), NOT a predictive alpha-beta.
 * A velocity/momentum term overshoots on jerky hand-held motion (it predicts the
 * target keeps moving, then it reverses), which reads as "wobble". Measured: an
 * alpha-beta lags 2 px and overshoots 10 px on a jerky pan; the raw detector is
 * firmer (3 px). So the displayed box just EMAs toward the measurement with no
 * momentum; velocity is derived only for coasting + the fusion wire, never fed
 * back into the displayed position while a measurement is present.
 *   - at 15 fps (detector tick) you cannot beat the raw detector, so smooth lightly;
 *   - at 60 fps (the engaged lock) small per-frame moves let a firmer gain kill the
 *     jitter with almost no lag - this is why the lock must drive the output. */
/* ADAPTIVE gain: g = clamp(gmin + slope*innovation, gmin, gmax). Small innovation
 * (a still target with only detector jitter) -> low gain -> smooth, firm box. Large
 * innovation (the target moving under a camera pan) -> high gain -> follows with low
 * lag. No velocity term anywhere, so it never overshoots on a reversal. Measured:
 * this is firmer than a fixed gain when static AND lower-lag when moving. */
#define TRK_OUT_G_MIN           0.35    /* det-tick (15 fps) smooth-when-still floor */
#define TRK_OUT_G_SLOPE         0.05    /* per-px responsiveness */
#define TRK_OUT_G_MAX           0.85
#define TRK_OUT_GL_MIN          0.30    /* 60 fps lock: small per-frame moves -> smoother */
#define TRK_OUT_GL_SLOPE        0.06
#define TRK_OUT_GL_MAX          0.80
#define TRK_OUT_VEL_G           0.30    /* velocity-estimate EMA (coast + wire only) */
#define TRK_LOCK_HOLD_TICKS     2       /* det ticks the lock keeps output ownership */

/* Class is a SOFT association penalty, not a hard gate (a COCO placeholder flickers
 * human<->vehicle at range; a hard gate fragments tracks). Hard class stickiness
 * applies only to the engaged track. */
#define TRK_CLASS_PENALTY       0.4     /* fraction of gate charged for a class mismatch */

/* Engaged-target 60 fps lock loop (lock.c). Multi-scale NCC on a small ROI of the
 * raw Y10 frame; template rebuilt only from class-consistent NN detections. */
#define TRK_LOCK_ROI_MAX        192     /* max ROI side (px); template is <= this */
#define TRK_LOCK_SEARCH         24      /* +/- search radius around the prediction (px) */
#define TRK_LOCK_SCORE_MIN      0.50    /* NCC below this = no match (avoid latching onto
                                           low-texture background as the target leaves) */
#define TRK_LOCK_META_HOLD      4       /* frames to freeze template after an AE/illum step */

#endif /* TRK_CONFIG_H */
