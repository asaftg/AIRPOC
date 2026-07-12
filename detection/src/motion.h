/* motion.h — CPU motion worker: the safety net that catches ANY moving target
 * the appearance model missed, at any size (a far tiny drone, but equally a
 * mid-range vehicle or a person lost to poor contrast/shade/low light).
 *
 * Runs on a downscaled luma image. It keeps a ROLLING BACKGROUND — the per-pixel
 * median of the last few seconds of frames — and reports what deviates from it:
 *   background  = median(recent frames)          "what this pixel normally is"
 *   diff        = |current - background|          only genuine change survives
 *   destripe    = subtract each row's median      kills the night row read-noise
 *   confirm     = M-of-N over a ~1 s tracker       rejects sensor sparkle/twinkle
 * Reports movers in FULL-res pixel coordinates; the caller suppresses any that
 * overlap an appearance box. Class-less by design (a vehicle/drone is the same
 * kind of mover) — classification is the model's / fusion's job.
 *
 * Why a rolling background, not consecutive-frame difference: at night the frame-
 * to-frame difference is dominated by row read-noise and illuminated-foliage/cone
 * shimmer, which drowns a far walker. A background model subtracts the static scene
 * (parked cars, lit trees, distant lights) and leaves the transient mover.
 *
 * Static vs moving platform: the background is built in the current frame. On a
 * static/holding mount this is exact. On a slewing gimbal the frames must first be
 * ego-motion-aligned — that is the stab.h hook (identity today; IMU/VIO or ECC
 * later, with no change here). Until real ego-motion is wired, run motion on a
 * static/holding mount (see DET_MOTION_DEFAULT).
 */
#ifndef DET_MOTION_H
#define DET_MOTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float cx, cy, w, h;   /* full-res pixels, centre form */
    float conf;           /* confirmation hits / window */
    int   age;            /* frames since first seen */
} Mover;

/* Runtime motion parameters (from the daemon's live knobs + measured fps). */
typedef struct {
    double k_mad;     /* threshold = median(|diff|) + k_mad*MAD of the frame */
    double window_s;  /* rolling-background window in SECONDS (the GUI slider) */
    double fps;       /* measured capture rate — sizes the window & confirm in frames */
    int    persist;   /* 1..5: confirmation strength (fraction of the ~1 s M-of-N window) */
} MotionParams;

typedef struct MotionWorker MotionWorker;

/* down = spatial downscale factor (e.g. 4 => 1440x1088 -> 360x272).
 * use_ecc = 1 for ECC stabilization, 0 for the identity stub. */
MotionWorker *motion_new(int full_w, int full_h, int down, int use_ecc);

/* Process one full-res Y10 frame against the rolling background. Writes up to
 * max_out movers, returns the count; *stab_fail (if non-NULL) set to 1 if
 * alignment failed. Returns 0 while the background is still warming up. */
int motion_process(MotionWorker *, const uint8_t *y10, int w, int h,
                   const MotionParams *p, Mover *out, int max_out, int *stab_fail);

void motion_free(MotionWorker *);

#ifdef __cplusplus
}
#endif

#endif /* DET_MOTION_H */
