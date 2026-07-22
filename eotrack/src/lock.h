/* lock.h - the engaged-target 60 fps lock loop. Sparse pyramidal Lucas-Kanade optical
 * flow, hand-rolled in C (no OpenCV, no GPU - the GPU is the detector's and is reserved
 * for future terrain-nav).
 *
 * Model: the lock holds its own tracked centre. Each detector tick the caller RE-ANCHORS
 * it to the fresh detection (lock_anchor: snap centre, detect corner points in the box,
 * store the frame). Every 60 fps frame between detections the caller advances it by
 * optical flow (lock_track: track the corner points into the new frame, take the robust
 * median translation). So the detector is the drift anchor and LK is the between-tick
 * filler - LK never integrates for more than the ~4 frames between detections.
 *
 * Guardrails (per the design):
 *  - the DETECTOR (not a self-update) re-seeds position + points every tick -> no drift;
 *  - the forward-backward consistency check discards points that don't track cleanly,
 *    so a shake or a distractor edge cannot pull the median off the target;
 *  - the caller HOLDs (no flow) across an AE/illuminator step (brightness-constancy is
 *    violated) and re-anchors on the next detection;
 *  - a low surviving-point fraction is reported as a low score -> the caller HOLDs rather
 *    than jumping the box onto background.
 */
#ifndef TRK_LOCK_H
#define TRK_LOCK_H

#include <stdint.h>

typedef struct Lock Lock;

Lock *lock_new(void);
void  lock_free(Lock *l);
void  lock_reset(Lock *l);                 /* drop points + prev frame (disengage / step) */
int   lock_has_template(const Lock *l);    /* 1 if it holds tracked points + a prev frame */

/* Re-anchor to a fresh detection: set the tracked centre to (cx,cy), detect corner points
 * inside the box (cx,cy,bw,bh) of `frame`, and store `frame` as the previous frame. Called
 * every detector tick (the detector is ground truth for where the target is). */
void  lock_anchor(Lock *l, const uint16_t *frame, int w, int h,
                  double cx, double cy, double bw, double bh);

/* Advance one 60 fps frame by optical flow: track the stored points from the previous
 * frame into `frame`, using (ego_dx,ego_dy) - the whole-scene camera shift this frame - as
 * the initial flow guess. Writes the new tracked centre (ox,oy) and a score in [0,1] = the
 * fraction of points that tracked consistently (forward-backward-clean). Returns 1 on a
 * usable result, 0 if it holds no points/prev frame. A score below the caller's threshold
 * means HOLD (do not move the box), not a hard loss - the detector give-up timer decides
 * loss. */
int   lock_track(Lock *l, const uint16_t *frame, int w, int h,
                 double ego_dx, double ego_dy,
                 double *ox, double *oy, double *score);

#endif /* TRK_LOCK_H */
