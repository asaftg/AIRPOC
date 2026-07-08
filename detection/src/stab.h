/* stab.h — frame-alignment interface for the motion worker.
 *
 * A stabilizer estimates the 2x3 affine warp that maps the PREVIOUS down-res luma
 * frame onto the CURRENT one, so subtracting the aligned previous frame cancels
 * the platform's own motion and leaves only real movers. Today: identity (stub)
 * or image-based ECC. Later: real IMU/VIO ego-motion behind this same interface,
 * with no change to the motion worker.
 *
 * warp is row-major [a,b,tx, c,d,ty]; applying it to prev aligns prev onto cur.
 */
#ifndef DET_STAB_H
#define DET_STAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { STAB_OK = 0, STAB_DEGRADED = 1, STAB_FAIL = 2 };

typedef struct {
    int  (*align)(void *self, const uint8_t *prev, const uint8_t *cur, float warp[6]);
    void (*destroy)(void *self);
    void *self;
} Stabilizer;

Stabilizer stab_identity_new(int w, int h);
Stabilizer stab_ecc_new(int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* DET_STAB_H */
