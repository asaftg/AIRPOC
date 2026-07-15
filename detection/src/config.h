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

#define DET_VERSION       "0.4.0"          /* native rolling-background motion on cadence + contract */

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
 * cadence default runs the (future) GPU detector at every 4th frame (~15/s when
 * capture is 60/s); it is meant to be raised toward 1 (every frame) as a target
 * closes — a fast crosser up close needs the higher rate. */
#define DET_CONF_DEFAULT       0.5     /* safe default: the stock model floods low-conf
                                          on cluttered/out-of-domain scenes; operator
                                          tunes per scene via /ctl */
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
#define DET_MOTION_DEFAULT     0       /* motion worker OFF by default: the rolling
                                          background is built in the current frame, so
                                          on a MOVING camera it needs ego-motion
                                          alignment (IMU/VIO, or ECC -E) behind
                                          stabilize() first. Enable via /ctl on a
                                          static/holding mount or once ego-motion is real. */
#define DET_MAXDETS_DEFAULT    128
#define DET_MAXDETS_MIN        1
#define DET_MAXDETS_MAX        512
#define DET_MOT_K_DEFAULT      6.0     /* MAD multiplier for the motion threshold */
#define DET_MOT_K_MIN          1.0
#define DET_MOT_K_MAX          30.0
#define DET_MOT_WINDOW_S_DEFAULT 15.0  /* rolling-background window (seconds): how far back
                                          "normal scene" is modelled. Short adapts fast &
                                          is cleaner in a changing scene; long is smoother
                                          but slower to forget a stopped object. GUI slider. */
#define DET_MOT_WINDOW_S_MIN   1.0
#define DET_MOT_WINDOW_S_MAX   60.0
#define DET_MOT_PERSIST_DEFAULT 3      /* confirmation strength 1..5 = fraction of the ~1 s
                                          M-of-N tracker window that must hit before a mover
                                          is reported (rejects sparkle/twinkle) */
#define DET_MOT_PERSIST_MIN    1
#define DET_MOT_PERSIST_MAX    5
#define DET_MOT_DOWN_DEFAULT   1       /* motion spatial downscale: 1 = NATIVE (resolves the far/
                                          small movers the net exists to catch). Higher = cheaper
                                          but blinds small targets — 4 collapses a far human to
                                          ~3 px. Trade CPU with `cadence`, not by throwing away pixels. */
#define DET_MOT_DOWN_MIN       1
#define DET_MOT_DOWN_MAX       4
/* The motion worker runs on the SAME `cadence` tick as the appearance model (one rate
 * for both) — far/small movers are slow in pixels and don't need the full camera rate,
 * and that lower rate is what makes native resolution affordable. No separate motion
 * rate knob: raise/lower `cadence` and both detectors follow. */

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
