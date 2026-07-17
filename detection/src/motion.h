/* motion.h — CPU motion worker: the safety net that catches ANY moving target
 * the appearance model missed, at any size (a far tiny drone, but equally a
 * mid-range vehicle or a person lost to poor contrast/shade/low light).
 *
 * Runs on a (possibly downscaled) luma image and reports what moved relative to a
 * reference frame, then confirms it over time:
 *   reference   = a "no-mover" view of the scene           (two ways to get it, below)
 *   diff        = |current - reference|                     only genuine change survives
 *   destripe    = subtract each row's median                kills the night row read-noise
 *   confirm     = M-of-N over a ~1 s tracker                rejects sensor sparkle/twinkle
 * Reports movers in FULL-res pixel coordinates; the caller suppresses any that
 * overlap an appearance box. **Deliberately permissive** — its job is to not MISS a
 * mover, not to be clean; the EO tracker's temporal-integration (keep tracks that
 * translate, drop those that oscillate in place, e.g. wind-blown foliage) is what
 * removes clutter. Class-less by design; classification is the model's / fusion's job.
 *
 * Two reference methods (mot_method), sharing everything downstream — the tracker
 * phase picks the winner on real data:
 *   0  BACKGROUND-SUBTRACTION: reference = per-pixel MEDIAN of the last window_s.
 *      Best on a stable scene with high-contrast movers (a bright far walker on dark
 *      road). Pitfall: it ABSORBS a slow / near-stationary target that lingers in view
 *      (the median becomes the target), and it is the more expensive path (median rebuild).
 *   1  FRAME-DIFFERENCE: reference = the frame ~baseline_s ago. Catches slow persistent
 *      movers the median absorbs; cheaper (no median). Pitfall: too-short a baseline
 *      misses a slow far target (tiny per-baseline displacement); a long baseline over a
 *      moving camera smears unless ego-motion-aligned.
 *
 * Static vs moving platform: the reference is compared in the current frame. On a
 * static/holding mount this is exact. On a slewing gimbal the reference must first be
 * ego-motion-aligned — that is the stab.h hook (identity today; IMU/VIO or ECC later,
 * with no change here). Until real ego-motion is wired, run motion on a static/holding
 * mount (see DET_MOTION_DEFAULT).
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
    double k_mad;      /* threshold = median(|diff|) + k_mad*MAD of the frame */
    double window_s;   /* method 0: rolling-background window in SECONDS */
    double fps;        /* measured process rate — sizes the windows in frames */
    int    persist;    /* 1..5: confirmation strength (fraction of the ~1 s M-of-N window) */
    int    method;     /* 0 = background-subtraction, 1 = frame-difference */
    double baseline_s; /* method 1: difference against the frame this many SECONDS back */
} MotionParams;

typedef struct MotionWorker MotionWorker;

/* down = spatial downscale factor (1 = native; e.g. 4 => 1440x1088 -> 360x272).
 * use_ecc = 1 for ECC stabilization, 0 for the identity stub. */
MotionWorker *motion_new(int full_w, int full_h, int down, int use_ecc);

/* Process one full-res Y10 frame. Writes up to max_out movers, returns the count;
 * *stab_fail (if non-NULL) set to 1 if alignment failed. Returns 0 while the
 * reference is still warming up. */
int motion_process(MotionWorker *, const uint8_t *y10, int w, int h,
                   const MotionParams *p, Mover *out, int max_out, int *stab_fail);

void motion_free(MotionWorker *);

#ifdef __cplusplus
}
#endif

#endif /* DET_MOTION_H */
