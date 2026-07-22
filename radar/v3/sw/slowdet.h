/* slowdet — slow radar detector (short-hop temporal chaining) for AIRPOC.
 *
 * Complements the cluster tracker (cluster.c): it chains faint, intermittent
 * echoes frame-to-frame over short bounded hops and declares a target once the
 * chain is long and coherent enough. This is what catches the far/faint movers
 * that never form a confident per-frame cluster — the 300 m car that shows an
 * echo only ~60% of frames, the outbound walker past 240 m.
 *
 * Class-less, like cluster: emits RadarTarget boxes; person/vehicle labelling is
 * the fusion module's job. Velocity is POSITION-derived (never ambiguous
 * Doppler). Fixed ring buffers, no allocation on the hot path.
 *
 * Validated offline against the fixture corpus (walk / longnight positives;
 * static / c16 / coldboot negatives stay ~zero). See radar/tools.
 */
#ifndef AIRPOC_SLOWDET_H
#define AIRPOC_SLOWDET_H

#include "radar.h"

typedef struct SlowDet SlowDet;

SlowDet *slowdet_new(void);
void     slowdet_free(SlowDet *);

/* Detect on one frame. pts[0..n) are this frame's points (sensor frame, same
 * array cluster_step sees). now_s is a monotonic timestamp in seconds. Declared
 * slow targets are written to out[0..ret); ids are in a high band (>=SLOWDET_TID_BASE)
 * so they never collide with cluster's ids before the fuse stage reconciles them. */
int      slowdet_step(SlowDet *, const RadarPoint *pts, int n, double now_s,
                      RadarTarget *out, int max_out);

#define SLOWDET_TID_BASE 1000

#endif /* AIRPOC_SLOWDET_H */
