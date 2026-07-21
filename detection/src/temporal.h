/* temporal.h — track-before-detect (TBD) evidence integrator for the appearance model.
 *
 * WHY THIS EXISTS
 * The per-frame confidence threshold is a LOSSY GATE: a detection below it is
 * destroyed, and nothing downstream (EO tracker, fusion) can ever recover it. A far
 * or low-contrast target that the model scores at 0.2 every single frame is a real,
 * repeatable, physically-consistent signal — but a 0.5 gate throws it away frame after
 * frame. TBD integrates that evidence BEFORE the gate: run the model at a low floor,
 * accumulate the weak hits along a plausible short trajectory, and promote the target
 * once the accumulated evidence is strong enough. Sensitivity is bought with time
 * instead of with a lower (and therefore noisier) single-frame threshold.
 *
 * TWO TIERS, ONE EMIT PATH (no double boxes)
 * Every candidate — strong or weak — goes through this integrator; it is the single
 * place that decides what gets emitted. A track is emitted on a tick when it has an
 * observation on that tick AND either
 *   (a) that observation is already at/above the normal confidence `hi`  -> emitted
 *       immediately, with its raw confidence, exactly as before TBD existed. Strong
 *       targets therefore pay ZERO added latency and see zero behaviour change; or
 *   (b) its integrated score has reached `confirm`                       -> promoted.
 * Because emission is per-track and there is only one code path, a target can never
 * be reported twice (once as "strong" and once as "integrated") in the same tick.
 *
 * THE SCORE (sequential likelihood ratio)
 * On a hit:  S += presence + (logit(conf) - logit(lo))
 * On a miss: S -= miss_penalty
 * The logit difference is taken RELATIVE TO `lo`, the confidence at which a detection
 * is judged as likely clutter as target: a hit exactly at `lo` contributes only the
 * small fixed `presence` term (it happened at all, twice, in the same place), while a
 * 0.45 hit contributes much more. That is the whole point — this weights evidence
 * instead of merely counting it, so a few near-threshold looks confirm as fast as many
 * marginal ones. Misses subtract, so a flickering false alarm decays and dies while a
 * steady weak target climbs.
 *
 * WHAT IT IS NOT
 * This is not the EO tracker. It carries no identity, no appearance model, no
 * occlusion handling, no re-acquisition, and it never emits a predicted/coasted box —
 * only boxes the model actually produced on that tick, so the detector never invents
 * a target. The cross-frame link exists solely to decide "is this the same evidence
 * accumulating?", and is a gated nearest-neighbour with a constant-velocity predict.
 *
 * FREE OUTPUT FOR THE TRACKER PHASE
 * Each emitted box carries `hits`, `age` and `net_disp` (straight-line pixels from
 * where the track was first seen) alongside `path_len`. net_disp vs path_len is the
 * translate-vs-oscillate discriminator: wind-blown foliage accumulates a large path
 * length but returns to where it started (net_disp ~ 0), while a real target's
 * net_disp grows steadily. The tracker consumes these rather than recomputing them.
 */
#ifndef DET_TEMPORAL_H
#define DET_TEMPORAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* One per-frame model candidate fed in (already NMS'd, at the low floor `lo`). */
typedef struct {
    float       cx, cy, w, h;   /* full-res pixels */
    float       conf;           /* raw per-frame model confidence */
    const char *cls;            /* class name (static string), may be NULL */
} TbdIn;

/* One emitted detection. */
typedef struct {
    float       cx, cy, w, h;   /* THIS tick's observed box (never predicted) */
    float       conf;           /* raw conf if strong; else mapped from the score */
    const char *cls;
    int         age;            /* ticks since the track was first seen */
    int         hits;           /* observations accumulated */
    float       net_disp;       /* px, straight line from first observation */
    float       path_len;       /* px, summed observed steps */
    int         tbd;            /* 1 = promoted by integration (was below `hi`) */
} TbdOut;

typedef struct {
    double lo;            /* candidate floor: model runs at this, evidence reference */
    double hi;            /* immediate-emit confidence (the normal `conf` knob) */
    int    frames;        /* frames of evidence needed to report a target sitting exactly
                           * at `lo`. This is the operator-facing form of the promotion
                           * threshold: internally the score target is
                           * frames * DET_TBD_PRESENCE, so a target the model scores
                           * ABOVE lo confirms proportionally sooner. */
    double miss_penalty;  /* score subtracted per tick with no observation */
    int    max_miss;      /* consecutive missed ticks before a track is dropped */
    double fps;           /* detector TICK rate, used to scale the association gate */
} TbdParams;

typedef struct TbdCtx TbdCtx;

TbdCtx *tbd_new(void);
void    tbd_free(TbdCtx *c);
void    tbd_reset(TbdCtx *c);          /* drop all tracks (e.g. after a feed gap) */

/* Integrate this tick's candidates and return the boxes to emit.
 * Returns the number written to out[] (<= max_out). */
int     tbd_process(TbdCtx *c, const TbdIn *in, int n_in, const TbdParams *p,
                    TbdOut *out, int max_out);

/* Live tracks being carried (candidates under integration) — for /stats. */
int     tbd_live_tracks(const TbdCtx *c);

#ifdef __cplusplus
}
#endif

#endif /* DET_TEMPORAL_H */
