/* fuse — unify the cluster (F) and slowdet (S) target sets, and condition the
 * published elevation. AIRPOC radar module output stage.
 *
 *   MERGE      one box per object. F (the confirmed per-frame tracker) is
 *              authoritative; an S target is emitted only where no F target sits
 *              at the same range and bearing — S fills the far/faint gaps F
 *              cannot hold. Ids are preserved so a box does not flicker.
 *
 *   ELEVATION  the radar's up/down angle is ~3 deg noisy (11 deg at the tail)
 *              while range and azimuth are accurate. Azimuth (x,y) is left
 *              untouched; the elevation (z) of every published target — F and S
 *              alike — is passed through a trailing median whose window scales
 *              with range, with a physical rate limit, and its vertical box
 *              half-height is capped. Range and azimuth stay full-rate; only the
 *              noisy axis is smoothed. Display-independent: every consumer
 *              (GUI, recorder, fusion) gets the conditioned value.
 */
#ifndef AIRPOC_FUSE_H
#define AIRPOC_FUSE_H

#include "radar.h"

typedef struct Fuse Fuse;

Fuse *fuse_new(void);
void  fuse_free(Fuse *);

/* Merge F[0..nF) and S[0..nS) into out[0..ret), then smooth elevation in place.
 * now_s is a monotonic timestamp (seconds). */
int   fuse_step(Fuse *, const RadarTarget *F, int nF,
                const RadarTarget *S, int nS, double now_s,
                RadarTarget *out, int max_out);

#endif /* AIRPOC_FUSE_H */
