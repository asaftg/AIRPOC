/* ego.h - coarse global-motion (ego-motion) estimate between consecutive EO frames.
 *
 * The translate-vs-oscillate clutter test is camera-relative: on a panning gimbal
 * the whole scene translates and every static clutter patch reads as a real mover.
 * This estimates the frame-to-frame global shift (dx,dy in full px) from decimated
 * row/column intensity projections and their 1-D correlation. On a static mount it
 * returns ~0 and the clutter test is unchanged; on a pan it supplies the shift the
 * core subtracts from each track's stored path. When a real IMU/VIO exists it slots
 * in behind the same (dx,dy) output - this is the placeholder, not a dead end. */
#ifndef TRK_EGO_H
#define TRK_EGO_H

#include <stdint.h>

typedef struct Ego Ego;

Ego *ego_new(void);
void ego_free(Ego *e);

/* Feed the newest frame; outputs the estimated shift since the previous fed frame
 * (0,0 on the first frame or if confidence is low). */
void ego_update(Ego *e, const uint16_t *frame, int w, int h, double *dx, double *dy);

#endif /* TRK_EGO_H */
