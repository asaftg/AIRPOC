/* motion.h — CPU motion worker: the safety net that catches ANY moving target
 * the appearance model missed, at any size (a far tiny drone, but equally a
 * mid-range vehicle or a person lost to poor contrast/shade/low light).
 *
 * Runs on a downscaled luma image: aligns the previous frame onto the current one
 * (stab.h) to cancel platform motion, differences them, keeps changes above the
 * per-frame noise level, groups them into blobs, and only reports a blob that
 * persists across frames (rejects sensor sparkle). Reports movers in FULL-res
 * pixel coordinates; the caller suppresses any that overlap an appearance box.
 */
#ifndef DET_MOTION_H
#define DET_MOTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float cx, cy, w, h;   /* full-res pixels, centre form */
    float conf;           /* persistence hits / window */
    int   age;            /* frames since first seen */
} Mover;

typedef struct MotionWorker MotionWorker;

/* down = spatial downscale factor (e.g. 4 => 1440x1088 -> 360x272).
 * use_ecc = 1 for ECC stabilization, 0 for the identity stub. */
MotionWorker *motion_new(int full_w, int full_h, int down, int use_ecc);

/* Process one full-res Y10 frame. k_mad scales the noise threshold; persist is the
 * hits-in-last-5-frames required to report a blob. Writes up to max_out movers,
 * returns the count; *stab_fail (if non-NULL) set to 1 if alignment failed. */
int motion_process(MotionWorker *, const uint8_t *y10, int w, int h,
                   double k_mad, int persist, Mover *out, int max_out, int *stab_fail);

void motion_free(MotionWorker *);

#ifdef __cplusplus
}
#endif

#endif /* DET_MOTION_H */
