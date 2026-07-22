/* core.c - EO tracker core. See core.h and the module README for the design.
 *
 * Pipeline per tick:
 *   1. predict every track forward by dt (constant-velocity)
 *   2. build all (track, det) candidate pairs inside the gate, sorted by distance,
 *      and claim greedily in ascending-distance order - confirmed tracks first, so
 *      a flood of junk cannot steal an established target (two-tier, cf. cluster.c)
 *   3. update matched tracks (filter + evidence + history), spawn tracks for the
 *      leftover detections, age/coast/kill the unmatched
 *   4. run the translate-vs-oscillate clutter test over the horizon and set each
 *      track's emission latch (kept internally either way; only latch-passing,
 *      confirmed/coasting tracks are written to the wire -> E is a subset of live)
 */
#include "core.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    double t;            /* seconds (from t_src, monotone within a run) */
    double x, y;         /* ego-compensated centre, px */
} HistPt;

typedef struct {
    int      used;
    int      tid;
    /* filtered association state (px, IMAGE frame) */
    double   cx, cy, w, h;
    double   cx_prev, cy_prev;   /* image position before this tick's predict (for world vel) */
    double   vx, vy;             /* target's own (WORLD) velocity, px/s (camera motion removed) */
    /* smoothed output state (firm position EMA; velocity is coast/wire only) */
    double   ocx, ocy, ovx, ovy;
    double   meas_cx, meas_cy;   /* last matched detection centre (output EMA target) */
    int      lock_hold;          /* >0: the 60 fps lock owns the output this many det ticks */
    /* evidence / lifecycle */
    double   score;
    int      confirmed;          /* 0 tentative, 1 confirmed (claims points first) */
    int      hits_total;
    int      misses;
    double   age_s;
    double   conf;               /* EMA of detection confidence */
    /* class vote */
    int      cls_votes[4];
    int      cls;
    /* source mix */
    int      app_hits, mot_hits;
    /* size growth (looming), relative 1/s, EMA */
    double   grow;
    /* clutter test */
    HistPt   hist[TRK_HIST];
    int      hn, hhead;
    int      emit_latch;         /* 1 = passes translate test (emit), 0 = latched off */
    double   net_over_path;      /* last computed ratio (diagnostic) */
    /* position sigma (px), EMA of innovation magnitude */
    double   sigma;
    /* timing */
    uint64_t t_meas_ns;          /* t_src of last real measurement */
    double   coast_s;
    int      lock_on;
    double   lock_score;
} Track;

struct TrkCore {
    Track    tr[TRK_MAX_TRACKS];
    int      next_tid;
    double   clock_s;            /* accumulated time from dt (monotone) */
    TrkKnobs k;
    double   meas_fps;           /* EMA of measured tick rate */
    int      live, emitted;
};

typedef struct { int ti, di; double d2; } Pair;

static int emit_tracks(TrkCore *c, int engaged_tid, TrkOut *out, int max);

static double clampd(double v, double lo, double hi){ return v<lo?lo:v>hi?hi:v; }

TrkCore *trk_core_new(void)
{
    TrkCore *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->next_tid = 1;
    c->meas_fps = TRK_GATE_REF_FPS;
    c->k.gate_base = TRK_GATE_BASE_DEFAULT;
    c->k.confirm   = TRK_CONFIRM_DEFAULT;
    c->k.coast_s   = TRK_COAST_S_DEFAULT;
    c->k.clutter_s = TRK_CLUTTER_S_DEFAULT;
    return c;
}

void trk_core_free(TrkCore *c){ free(c); }

void trk_core_set_knobs(TrkCore *c, const TrkKnobs *k)
{
    c->k.gate_base = clampd(k->gate_base, TRK_GATE_BASE_MIN, TRK_GATE_BASE_MAX);
    c->k.confirm   = clampd(k->confirm,   TRK_CONFIRM_MIN,   TRK_CONFIRM_MAX);
    c->k.coast_s   = clampd(k->coast_s,   TRK_COAST_S_MIN,   TRK_COAST_S_MAX);
    c->k.clutter_s = clampd(k->clutter_s, TRK_CLUTTER_S_MIN, TRK_CLUTTER_S_MAX);
}

static Track *alloc_track(TrkCore *c)
{
    Track *weakest = NULL;
    for (int i = 0; i < TRK_MAX_TRACKS; i++) {
        if (!c->tr[i].used) { memset(&c->tr[i], 0, sizeof(Track)); return &c->tr[i]; }
        /* pool full: remember the weakest tentative to evict */
        if (!c->tr[i].confirmed &&
            (!weakest || c->tr[i].score < weakest->score)) weakest = &c->tr[i];
    }
    if (weakest) { memset(weakest, 0, sizeof(Track)); return weakest; }
    return NULL;    /* all confirmed and full - drop the new detection */
}

static void hist_push(Track *t, double clk, double x, double y)
{
    t->hist[t->hhead].t = clk;
    t->hist[t->hhead].x = x;
    t->hist[t->hhead].y = y;
    t->hhead = (t->hhead + 1) % TRK_HIST;
    if (t->hn < TRK_HIST) t->hn++;
}

/* Translate-vs-oscillate: over the clutter horizon, net displacement / path length.
 * Oscillators (foliage) have long path, ~zero net. Approachers net ~zero but are
 * rescued by looming (size growth) or a classified model hit. */
static void clutter_eval(Track *t, double clk, double horizon)
{
    if (t->hn < 3) { t->emit_latch = 1; t->net_over_path = 1.0; return; }
    /* walk the ring newest->oldest within the horizon */
    double path = 0.0;
    double nx = 0, ny = 0;      /* newest */
    double ox = 0, oy = 0;      /* oldest in window */
    int have_new = 0, count = 0;
    double px = 0, py = 0;
    for (int k = 0; k < t->hn; k++) {
        int idx = (t->hhead - 1 - k + 2 * TRK_HIST) % TRK_HIST;
        HistPt *p = &t->hist[idx];
        if (clk - p->t > horizon) break;
        if (!have_new) { nx = p->x; ny = p->y; have_new = 1; }
        else { path += hypot(px - p->x, py - p->y); }
        px = p->x; py = p->y;
        ox = p->x; oy = p->y;
        count++;
    }
    if (count < 3 || path < 1e-6) { t->emit_latch = 1; t->net_over_path = 1.0; return; }
    double net = hypot(nx - ox, ny - oy);
    double ratio = net / path;
    t->net_over_path = ratio;

    int translating = ratio >= TRK_TRANS_RATIO;
    int looming     = t->grow >= TRK_LOOM_RATE;
    int classified  = (t->cls >= 1);   /* the model gave it a real class */
    /* Emit if it clearly moves across the frame, OR is growing (radial approach),
     * OR the appearance model classifies it (a parked-but-real car/human). A pure
     * in-place oscillator with no class and no looming is latched off. */
    t->emit_latch = translating || looming || classified;
}

int trk_core_step(TrkCore *c, const TrkDet *dets, int n,
                  uint64_t t_src_ns, double dt_s,
                  double ego_dx, double ego_dy, int engaged_tid,
                  TrkOut *out, int max)
{
    if (dt_s <= 1e-4) dt_s = 1.0 / TRK_GATE_REF_FPS;
    if (dt_s > 2.0)   dt_s = 2.0;
    c->clock_s += dt_s;
    double clk = c->clock_s;
    /* measured rate EMA (drives the gate scale - MEASURED, not configured) */
    double inst_fps = 1.0 / dt_s;
    c->meas_fps += 0.2 * (inst_fps - c->meas_fps);
    double fps = c->meas_fps > 1.0 ? c->meas_fps : 1.0;
    double gate_scale = TRK_GATE_REF_FPS / fps;
    if (n > TRK_MAX_IN) n = TRK_MAX_IN;

    /* 1. predict every track's IMAGE position by the camera motion (ego) PLUS the
     * target's own world velocity. This is the key to tracking under a moving sensor:
     * a static object's box moves with the scene, so the detection lands inside the
     * gate next tick instead of spawning a duplicate while the old track coasts off.
     * (ego_dx,ego_dy) is the per-tick image shift of the static scene; vx,vy is the
     * target's motion beyond the camera (~0 for anything world-stationary). */
    for (int i = 0; i < TRK_MAX_TRACKS; i++) {
        Track *t = &c->tr[i];
        if (!t->used) continue;
        t->cx_prev = t->cx; t->cy_prev = t->cy;
        t->cx += ego_dx + t->vx * dt_s;
        t->cy += ego_dy + t->vy * dt_s;
        t->ocx += ego_dx;   /* move the smoothed output box with the camera too */
        t->ocy += ego_dy;
        /* history is kept WORLD-stabilised for the clutter test (remove the ego). */
        for (int k = 0; k < t->hn; k++) { t->hist[k].x -= ego_dx; t->hist[k].y -= ego_dy; }
        t->lock_on = 0;
    }

    /* 2. candidate pairs within gate, ascending distance */
    static Pair pairs[TRK_MAX_TRACKS * 8];
    int np = 0;
    for (int di = 0; di < n; di++) {
        const TrkDet *d = &dets[di];
        double dim = fmax(d->w, d->h);
        for (int i = 0; i < TRK_MAX_TRACKS; i++) {
            Track *t = &c->tr[i];
            if (!t->used) continue;
            /* Gate = base + a term that grows with the track's own UNCERTAINTY (sigma =
             * EMA of recent innovation). A well-tracked, near-stationary target has a
             * small sigma -> a tight gate, so it cannot grab a different object that
             * happens to pass within half its box (the bug that made a parked car snap
             * onto a passing car, spike its velocity, coast away, and get re-numbered).
             * A fast or freshly-uncertain track has a large sigma -> a wide gate, so it
             * still follows. A small box-size term keeps big objects from being gated
             * tighter than their own centre noise. */
            double gate = (c->k.gate_base + TRK_GATE_SIGMA_K * t->sigma + 0.15 * dim) * gate_scale;
            if (gate > TRK_GATE_MAX_PX * gate_scale) gate = TRK_GATE_MAX_PX * gate_scale;
            /* soft class penalty: shrink the gate on a class mismatch (movers cls=0
             * are class-less and never penalised) */
            if (d->cls >= 1 && t->cls >= 1 && d->cls != t->cls)
                gate *= (1.0 - TRK_CLASS_PENALTY);
            double dx = d->cx - t->cx, dy = d->cy - t->cy;
            double d2 = dx*dx + dy*dy;
            if (d2 <= gate*gate && np < (int)(sizeof pairs/sizeof pairs[0])) {
                pairs[np].ti = i; pairs[np].di = di; pairs[np].d2 = d2; np++;
            }
        }
    }
    /* insertion sort by distance then confirmed-first: process confirmed tracks'
     * pairs before tentative ones at equal distance so junk cannot outbid them */
    for (int a = 1; a < np; a++) {
        Pair key = pairs[a]; int b = a - 1;
        while (b >= 0 && pairs[b].d2 > key.d2) { pairs[b+1] = pairs[b]; b--; }
        pairs[b+1] = key;
    }

    char det_taken[TRK_MAX_IN];  memset(det_taken, 0, sizeof det_taken);
    char trk_taken[TRK_MAX_TRACKS]; memset(trk_taken, 0, sizeof trk_taken);

    /* pass 1: confirmed tracks claim, pass 2: tentative claim */
    for (int pass = 0; pass < 2; pass++) {
        for (int p = 0; p < np; p++) {
            int i = pairs[p].ti, di = pairs[p].di;
            Track *t = &c->tr[i];
            if (!t->used || trk_taken[i] || det_taken[di]) continue;
            if (pass == 0 && !t->confirmed) continue;
            if (pass == 1 && t->confirmed) continue;
            const TrkDet *d = &dets[di];
            /* --- update matched track --- */
            double innov = hypot(d->cx - t->cx, d->cy - t->cy);
            t->sigma += 0.3 * (innov - t->sigma);
            /* WORLD velocity: the target's image displacement this tick MINUS the camera
             * motion (ego). A world-stationary object under a pan reads ~0 here, so it is
             * never coasted off along the camera's motion. */
            double wvx = ((d->cx - t->cx_prev) - ego_dx) / dt_s;
            double wvy = ((d->cy - t->cy_prev) - ego_dy) / dt_s;
            t->vx += 0.4 * (wvx - t->vx);
            t->vy += 0.4 * (wvy - t->vy);
            /* size growth (relative, 1/s) before overwriting w/h */
            double oldsz = 0.5 * (t->w + t->h);
            double newsz = 0.5 * (d->w + d->h);
            if (oldsz > 1.0) {
                double g = (newsz - oldsz) / (oldsz * dt_s);
                t->grow += 0.3 * (g - t->grow);
            }
            /* position/size EMA */
            t->cx += 0.6 * (d->cx - t->cx);
            t->cy += 0.6 * (d->cy - t->cy);
            t->w  += 0.5 * (d->w  - t->w);
            t->h  += 0.5 * (d->h  - t->h);
            t->conf += 0.4 * (d->conf - t->conf);
            /* evidence: strong direct det ~1.0; weak scaled by conf; tbd already
               integrated upstream so it counts fully. Hygiene, not sensitivity. */
            double add = d->tbd ? 1.0 : clampd(d->conf, 0.1, 1.0);
            if (d->src == 1) add *= 0.5;     /* movers are lighter evidence */
            t->score = clampd(t->score + add, TRK_SCORE_FLOOR, TRK_SCORE_MAX);
            if (d->cls >= 0 && d->cls <= 3) t->cls_votes[d->cls]++;
            if (d->src == 1) t->mot_hits++; else t->app_hits++;
            t->hits_total++;
            t->misses = 0;
            t->t_meas_ns = t_src_ns;
            t->meas_cx = d->cx; t->meas_cy = d->cy;   /* firm output EMA targets this */
            hist_push(t, clk, t->cx, t->cy);
            det_taken[di] = 1; trk_taken[i] = 1;
        }
    }

    /* 3a. spawn tracks for leftover detections */
    for (int di = 0; di < n; di++) {
        if (det_taken[di]) continue;
        const TrkDet *d = &dets[di];
        Track *t = alloc_track(c);
        if (!t) continue;
        t->used = 1;
        t->tid = c->next_tid++;
        t->cx = d->cx; t->cy = d->cy; t->w = d->w; t->h = d->h;
        t->meas_cx = d->cx; t->meas_cy = d->cy;
        t->vx = t->vy = 0;
        t->conf = d->conf;
        t->score = d->tbd ? 1.0 : clampd(d->conf, 0.1, 1.0);
        if (d->cls >= 0 && d->cls <= 3) t->cls_votes[d->cls]++;
        if (d->src == 1) t->mot_hits++; else t->app_hits++;
        t->hits_total = 1;
        t->sigma = 0.5 * fmax(d->w, d->h);
        t->t_meas_ns = t_src_ns;
        t->emit_latch = 1;
        hist_push(t, clk, t->cx, t->cy);
    }

    /* 3b. age unmatched, confirm/coast/kill, finalise class + clutter latch */
    int coast_frames = (int)(c->k.coast_s * fps + 0.5);
    if (coast_frames < 1) coast_frames = 1;
    for (int i = 0; i < TRK_MAX_TRACKS; i++) {
        Track *t = &c->tr[i];
        if (!t->used) continue;
        t->age_s += dt_s;
        if (!trk_taken[i]) {
            t->misses++;
            t->score = t->score - TRK_SCORE_MISS;
            /* coast: keep position moving on held velocity; sigma grows */
            t->sigma += 0.5 * dt_s * fps;
            /* An operator-engaged track is STICKY: the global coast budget never kills
             * it. It coasts (held velocity, growing coast_s) until the operator releases
             * the lock or the lock loop re-anchors it, so a locked target is always
             * drawable and its id never dangles. Everything else dies normally, EXCEPT a
             * stationary confirmed track gets park-hold (keeps its id through a blink). */
            if (t->tid != engaged_tid) {
                if (!t->confirmed && t->score <= TRK_SCORE_FLOOR) { t->used = 0; continue; }
                if (t->confirmed) {
                    int stationary = hypot(t->ovx, t->ovy) < TRK_STATIONARY_VEL;
                    int budget = stationary ? (int)(TRK_PARK_S * fps + 0.5) : coast_frames;
                    if (t->misses > budget) { t->used = 0; continue; }
                }
            }
        }
        if (!t->confirmed && t->score >= c->k.confirm) t->confirmed = 1;
        /* majority class */
        int best = 0, bi = 0;
        for (int k = 0; k < 4; k++) if (t->cls_votes[k] > best) { best = t->cls_votes[k]; bi = k; }
        t->cls = bi;
        t->coast_s = (double)t->misses / fps;
        clutter_eval(t, clk, c->k.clutter_s);

        /* Firm output position. Seed on first pass. */
        if (t->ocx == 0 && t->ocy == 0) { t->ocx = t->cx; t->ocy = t->cy; }
        if (t->lock_hold > 0) {
            /* the 60 fps lock owns this engaged track's output (updated in
             * trk_core_lock_update); the det tick must not fight it. */
            t->lock_hold--;
        } else if (trk_taken[i]) {
            /* measured this tick: firm ADAPTIVE EMA toward the raw detection centre - NO
             * velocity momentum, so it never overshoots on a jerky pan; the gain rises
             * with the move so it stays smooth when still and responsive when moving.
             * (The box was already moved by the camera in step 1, so this EMA only has
             * to absorb the target's own motion + jitter.) */
            double innov = hypot(t->meas_cx - t->ocx, t->meas_cy - t->ocy);
            double g = TRK_OUT_G_MIN + TRK_OUT_G_SLOPE * innov;
            if (g > TRK_OUT_G_MAX) g = TRK_OUT_G_MAX;
            t->ocx += g * (t->meas_cx - t->ocx);
            t->ocy += g * (t->meas_cy - t->ocy);
            /* smoothed WORLD velocity for coasting + the fusion wire (the camera is
             * handled in step 1, so this must be world-frame, not image). */
            t->ovx += TRK_OUT_VEL_G * (t->vx - t->ovx);
            t->ovy += TRK_OUT_VEL_G * (t->vy - t->ovy);
        } else {
            /* coasting: the camera motion was already applied in step 1; add only the
             * target's own (world) motion, so a world-stationary target coasts IN PLACE
             * on the object instead of sliding off along the camera pan. */
            t->ocx += t->ovx * dt_s;
            t->ocy += t->ovy * dt_s;
        }
    }

    /* 4. emit confirmed + latch-passing tracks */
    int no = emit_tracks(c, engaged_tid, out, max);
    return no;
}

/* fraction of the SMALLER box covered by its intersection with the other (center form). */
static double box_containment(const Track *a, const Track *b)
{
    double ax1 = a->ocx - a->w/2, ay1 = a->ocy - a->h/2, ax2 = a->ocx + a->w/2, ay2 = a->ocy + a->h/2;
    double bx1 = b->ocx - b->w/2, by1 = b->ocy - b->h/2, bx2 = b->ocx + b->w/2, by2 = b->ocy + b->h/2;
    double ix = fmin(ax2, bx2) - fmax(ax1, bx1);
    double iy = fmin(ay2, by2) - fmax(ay1, by1);
    if (ix <= 0 || iy <= 0) return 0;
    double inter = ix * iy;
    double aa = a->w * a->h, ab = b->w * b->h;
    double mn = fmin(aa, ab);
    return mn > 0 ? inter / mn : 0;
}

static int emit_tracks(TrkCore *c, int engaged_tid, TrkOut *out, int max)
{
    /* Collect emittable tracks, strongest first, then drop any that heavily overlaps a
     * stronger same-class one already kept. The detector fragments a big/close object
     * into several boxes (whole car + window + body); without this the operator sees a
     * stack of boxes with churning ids on one target. Strength = lifetime hits; the
     * engaged track is always kept and never suppressed. Mirrors the radar tracker's
     * spatial dedup. */
    int live = 0;
    int idx[TRK_MAX_TRACKS], ni = 0;
    for (int i = 0; i < TRK_MAX_TRACKS; i++) {
        Track *t = &c->tr[i];
        if (!t->used) continue;
        live++;
        int is_eng = (t->tid == engaged_tid);
        if (!is_eng && (!t->confirmed || !t->emit_latch)) continue;   /* E subset of live */
        idx[ni++] = i;
    }
    /* sort strongest-first: engaged wins, then more hits */
    for (int a = 1; a < ni; a++) {
        int k = idx[a], b = a - 1;
        while (b >= 0) {
            Track *tk = &c->tr[k], *tb = &c->tr[idx[b]];
            int k_eng = (tk->tid == engaged_tid), b_eng = (tb->tid == engaged_tid);
            int k_better = k_eng > b_eng || (k_eng == b_eng && tk->hits_total > tb->hits_total);
            if (!k_better) break;
            idx[b+1] = idx[b]; b--;
        }
        idx[b+1] = k;
    }
    int no = 0;
    char dropped[TRK_MAX_TRACKS]; memset(dropped, 0, ni);
    for (int a = 0; a < ni; a++) {
        Track *t = &c->tr[idx[a]];
        int is_eng = (t->tid == engaged_tid);
        if (!is_eng) {
            for (int b = 0; b < a; b++) {
                if (dropped[b]) continue;
                Track *s = &c->tr[idx[b]];
                if (s->cls == t->cls && box_containment(s, t) > TRK_DEDUP_CONTAIN) { dropped[a] = 1; break; }
            }
        }
        if (dropped[a]) continue;
        if (no >= max) break;
        TrkOut *o = &out[no++];
        o->tid = t->tid;
        o->state = t->misses > 0 ? "coast" : (t->confirmed ? "conf" : "tent");
        o->cls = t->cls;
        o->cls_conf = (t->hits_total > 0)
                    ? (double)((t->cls>=0&&t->cls<4)?t->cls_votes[t->cls]:0) / t->hits_total : 0;
        o->conf = t->conf;
        o->cx = t->ocx; o->cy = t->ocy; o->w = t->w; o->h = t->h;
        o->vx = t->ovx; o->vy = t->ovy;
        o->s_px = t->sigma;
        o->grow = t->grow;
        o->hits = t->hits_total;
        o->age_s = t->age_s;
        o->coast_s = t->coast_s;
        o->t_meas_ns = t->t_meas_ns;
        o->src = (t->app_hits && t->mot_hits) ? 2 : (t->mot_hits ? 1 : 0);
        o->lock_on = t->lock_on;
        o->lock_score = t->lock_score;
    }
    c->live = live; c->emitted = no;
    return no;
}

int trk_core_snapshot(TrkCore *c, int engaged_tid, TrkOut *out, int max)
{
    return emit_tracks(c, engaged_tid, out, max);
}

void trk_core_counts(const TrkCore *c, int *live, int *emitted)
{
    if (live) *live = c->live;
    if (emitted) *emitted = c->emitted;
}

int trk_core_has_track(const TrkCore *c, int tid)
{
    if (tid < 0) return 0;
    for (int i = 0; i < TRK_MAX_TRACKS; i++)
        if (c->tr[i].used && c->tr[i].tid == tid) return 1;
    return 0;
}

int trk_core_engaged_box(const TrkCore *c, int engaged_tid,
                         double *cx, double *cy, double *w, double *h, int *cls)
{
    for (int i = 0; i < TRK_MAX_TRACKS; i++) {
        const Track *t = &c->tr[i];
        if (t->used && t->tid == engaged_tid) {
            if (cx) *cx = t->cx;
            if (cy) *cy = t->cy;
            if (w) *w = t->w;
            if (h) *h = t->h;
            if (cls) *cls = t->cls;
            return 1;
        }
    }
    return 0;
}

void trk_core_lock_update(TrkCore *c, int engaged_tid,
                          double cx, double cy, double score, uint64_t t_src_ns)
{
    for (int i = 0; i < TRK_MAX_TRACKS; i++) {
        Track *t = &c->tr[i];
        if (!t->used || t->tid != engaged_tid) continue;
        t->cx = cx; t->cy = cy;                 /* association anchor for the next tick */
        /* Drive the OUTPUT at 60 fps with a firm EMA - this is what makes an engaged
         * lock feel firm: small per-frame moves, no velocity momentum, so the box sits
         * on the target under camera motion instead of the 15 fps stutter + overshoot. */
        if (t->ocx == 0 && t->ocy == 0) { t->ocx = cx; t->ocy = cy; }
        double innov = hypot(cx - t->ocx, cy - t->ocy);
        double g = TRK_OUT_GL_MIN + TRK_OUT_GL_SLOPE * innov;
        if (g > TRK_OUT_GL_MAX) g = TRK_OUT_GL_MAX;
        t->ocx += g * (cx - t->ocx);
        t->ocy += g * (cy - t->ocy);
        t->lock_hold = TRK_LOCK_HOLD_TICKS;     /* det tick yields output to the lock */
        t->t_meas_ns = t_src_ns;
        t->lock_on = 1;
        t->lock_score = score;
        if (t->misses > 0) t->misses = 0;       /* the lock is holding it */
        return;
    }
}
