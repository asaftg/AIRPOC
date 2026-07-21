/* temporal.c — track-before-detect evidence integrator. See temporal.h for the why.
 *
 * Cost is trivial next to inference: a few hundred float ops per candidate, no
 * allocation after tbd_new(), fixed-size track table. It runs in the main loop on the
 * detector tick, so it needs no locking.
 */
#include "temporal.h"
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int         used;
    unsigned    id;             /* see TbdOut.dtid — an evidence-pile tag, not identity */
    float       cx, cy, w, h;   /* last OBSERVED box */
    float       vx, vy;         /* px per tick, from consecutive observations */
    float       x0, y0;         /* first observation (for net displacement) */
    float       path_len;
    double      S;              /* accumulated log-likelihood-ratio score */
    int         age, hits, miss, since_obs;
    const char *cls;            /* class of the strongest observation so far */
    float       best_conf;
    float       last_conf;      /* this tick's observed confidence (0 = no hit) */
    int         hit_now;
} Trk;

struct TbdCtx {
    Trk      t[DET_TBD_MAX_TRACKS];
    unsigned next_id;
};

TbdCtx *tbd_new(void)
{
    TbdCtx *c = calloc(1, sizeof *c);
    return c;
}

void tbd_free(TbdCtx *c) { free(c); }

void tbd_reset(TbdCtx *c)
{
    if (c) memset(c->t, 0, sizeof c->t);
}

int tbd_live_tracks(const TbdCtx *c)
{
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < DET_TBD_MAX_TRACKS; i++) if (c->t[i].used) n++;
    return n;
}

/* Clamped log-odds. The clamp keeps a 0.0 or 1.0 model score from producing an
 * infinite jump in either direction. */
static double logit_c(double p)
{
    if (p < 0.02) p = 0.02;
    if (p > 0.98) p = 0.98;
    return log(p / (1.0 - p));
}

static void trk_start(Trk *t, const TbdIn *d, double lo_ref, unsigned id)
{
    t->used = 1;
    t->id = id;
    t->cx = d->cx; t->cy = d->cy; t->w = d->w; t->h = d->h;
    t->x0 = d->cx; t->y0 = d->cy;
    t->vx = t->vy = 0.0f;
    t->path_len = 0.0f;
    t->age = 0; t->hits = 1; t->miss = 0; t->since_obs = 0;
    t->cls = d->cls;
    t->best_conf = t->last_conf = d->conf;
    t->hit_now = 1;
    t->S = DET_TBD_PRESENCE + (logit_c(d->conf) - lo_ref);
}

static void trk_hit(Trk *t, const TbdIn *d, double lo_ref)
{
    float dx = d->cx - t->cx, dy = d->cy - t->cy;
    float step = sqrtf(dx * dx + dy * dy);
    float dt = (float)(t->since_obs > 0 ? t->since_obs : 1);

    /* Velocity from consecutive OBSERVATIONS, divided by the ticks between them, so a
     * gap doesn't inflate the estimate. Smoothed — a single noisy box shouldn't swing
     * the next tick's gate. */
    t->vx = 0.6f * t->vx + 0.4f * (dx / dt);
    t->vy = 0.6f * t->vy + 0.4f * (dy / dt);

    t->path_len += step;
    t->cx = d->cx; t->cy = d->cy; t->w = d->w; t->h = d->h;
    t->hits++; t->miss = 0; t->since_obs = 0;
    t->hit_now = 1; t->last_conf = d->conf;
    if (d->conf > t->best_conf) { t->best_conf = d->conf; t->cls = d->cls; }

    t->S += DET_TBD_PRESENCE + (logit_c(d->conf) - lo_ref);
    if (t->S > DET_TBD_S_MAX) t->S = DET_TBD_S_MAX;
}

int tbd_process(TbdCtx *c, const TbdIn *in, int n_in, const TbdParams *p,
                TbdOut *out, int max_out)
{
    if (!c || !p) return 0;

    const double fps     = p->fps > 0.5 ? p->fps : 15.0;
    const double lo_ref  = logit_c(p->lo);
    /* The knob is "frames of evidence at the floor"; the score target follows from it,
     * because a hit at exactly `lo` contributes exactly DET_TBD_PRESENCE. A stronger
     * hit contributes more, so it confirms in proportionally fewer frames. */
    const double confirm = (p->frames > 0 ? p->frames : 1) * DET_TBD_PRESENCE;
    /* Between ticks a target moves; the slower the tick rate the further. Scale the
     * gate by the tick interval relative to the nominal 15/s, and add the box size so
     * a large near object isn't gated tighter than its own extent. */
    const float  gate_scale = (float)(DET_TBD_GATE_REF_FPS / fps);

    /* --- predict --- */
    for (int i = 0; i < DET_TBD_MAX_TRACKS; i++) {
        Trk *t = &c->t[i];
        if (!t->used) continue;
        t->cx += t->vx; t->cy += t->vy;      /* constant-velocity predict */
        t->age++; t->since_obs++;
        t->hit_now = 0; t->last_conf = 0.0f;
    }

    /* --- associate, strongest candidate first ---
     * Greedy nearest-neighbour inside a gate. Strongest-first matters: when two
     * candidates contend for one track, the confident one should win it. */
    int order[DET_TBD_MAX_IN];
    int n = n_in > DET_TBD_MAX_IN ? DET_TBD_MAX_IN : n_in;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) {                       /* insertion sort, conf desc */
        int key = order[i], j = i - 1;
        while (j >= 0 && in[order[j]].conf < in[key].conf) { order[j + 1] = order[j]; j--; }
        order[j + 1] = key;
    }

    for (int oi = 0; oi < n; oi++) {
        const TbdIn *d = &in[order[oi]];
        float sz = d->w > d->h ? d->w : d->h;
        float gate = (DET_TBD_GATE_BASE_PX + sz) * gate_scale;
        float g2 = gate * gate;

        int best = -1;
        float bestd2 = g2;
        for (int i = 0; i < DET_TBD_MAX_TRACKS; i++) {
            Trk *t = &c->t[i];
            if (!t->used || t->hit_now) continue;       /* one observation per track */
            float dx = d->cx - t->cx, dy = d->cy - t->cy;
            float d2 = dx * dx + dy * dy;
            if (d2 < bestd2) { bestd2 = d2; best = i; }
        }

        if (best >= 0) {
            trk_hit(&c->t[best], d, lo_ref);
            continue;
        }
        /* unmatched -> new track in the first free slot (table full: drop, the
         * strongest candidates were served first) */
        for (int i = 0; i < DET_TBD_MAX_TRACKS; i++) {
            if (!c->t[i].used) { trk_start(&c->t[i], d, lo_ref, ++c->next_id); break; }
        }
    }

    /* --- misses: decay and retire --- */
    for (int i = 0; i < DET_TBD_MAX_TRACKS; i++) {
        Trk *t = &c->t[i];
        if (!t->used || t->hit_now) continue;
        t->miss++;
        t->S -= p->miss_penalty;
        if (t->miss > p->max_miss || t->S < DET_TBD_S_FLOOR) t->used = 0;
    }

    /* --- emit: the ONLY place a detection leaves the detector ---
     * One track -> at most one box, so a target cannot be reported twice. */
    int nout = 0;
    for (int i = 0; i < DET_TBD_MAX_TRACKS && nout < max_out; i++) {
        Trk *t = &c->t[i];
        if (!t->used || !t->hit_now) continue;          /* never emit a coasted box */

        int strong    = (t->last_conf >= (float)p->hi);
        int promoted  = (t->S >= confirm);
        if (!strong && !promoted) continue;

        TbdOut *o = &out[nout++];
        o->cx = t->cx; o->cy = t->cy; o->w = t->w; o->h = t->h;
        o->cls = t->cls;
        o->age = t->age;
        o->hits = t->hits;
        o->path_len = t->path_len;
        float ddx = t->cx - t->x0, ddy = t->cy - t->y0;
        o->net_disp = sqrtf(ddx * ddx + ddy * ddy);

        /* Confidence is ALWAYS the model's own score for this frame — for promoted
         * boxes exactly as much as for confident ones. It is deliberately NOT a
         * function of the accumulated score.
         *
         * An earlier version mapped the score onto [hi, 0.99] so that everything
         * emitted shared one scale. That was wrong: the score saturates within a few
         * frames, so every promoted box reported ~99% no matter how faint the model
         * actually found it — which makes a persistent false positive on a bush look
         * more certain than a real, clearly-seen vehicle. A number that is the same
         * for every box carries no information and actively misleads.
         *
         * How strong the accumulated evidence is, is a SEPARATE question from how
         * much the model recognises the thing, and it is already reported separately
         * and honestly as `hits` / `age` (with `tbd` saying the box exists only
         * because of them). Two truthful numbers beat one invented one. */
        o->conf = t->last_conf;
        o->tbd  = strong ? 0 : 1;
        o->dtid = t->id;
    }
    return nout;
}
