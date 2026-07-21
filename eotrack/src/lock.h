/* lock.h - the engaged-target 60 fps lock loop. A small hand-rolled multi-scale
 * normalised cross-correlation (NCC) on a fixed grid sampled across the target box.
 * No OpenCV, no GPU (the GPU is the detector's) - a fixed 24x24 correlation grid over
 * a +/-24 px search at 3 scales is well under a millisecond on one CPU core.
 *
 * Template policy (the classic-failure guardrails, per the design):
 *  - the template is rebuilt ONLY from a class-consistent NN detection box (position
 *    AND size), so the detector is the drift anchor and NCC is the between-tick filler
 *    - never a per-frame self-update that would drift;
 *  - 3 scales absorb a closing target's growth (a fixed-size template's score decays
 *    on exactly the engaged, closing targets that matter most);
 *  - the caller freezes template refresh across an AE/illuminator step (tap meta),
 *    and treats a low score as HOLD, not loss.
 */
#ifndef TRK_LOCK_H
#define TRK_LOCK_H

#include <stdint.h>

typedef struct Lock Lock;

Lock *lock_new(void);
void  lock_free(Lock *l);
void  lock_reset(Lock *l);                 /* drop the template (disengage / new target) */
int   lock_has_template(const Lock *l);

/* Rebuild the template by sampling a grid across the box (cx,cy,bw,bh) of `frame`. */
void  lock_set_template(Lock *l, const uint16_t *frame, int w, int h,
                        double cx, double cy, double bw, double bh);

/* Search near (px,py) for the template. On success returns 1 and writes the matched
 * centre (ox,oy) and NCC score [-1,1]. Returns 0 if no template. */
int   lock_track(Lock *l, const uint16_t *frame, int w, int h,
                 double px, double py, double *ox, double *oy, double *score);

#endif /* TRK_LOCK_H */
