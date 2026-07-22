/* core.c - fusion core. See core.h for the API and README for the design.
 *
 * Shape of a pass (runs on every input frame):
 *   ingest -> liveness -> re-bind scan (frame's own sensor) -> rows for new
 *   tids -> sustain fused pairs (divorce) -> candidate pairs (confirm) ->
 *   kill dead rows -> compose output.
 *
 * All state is static pools; the only allocation is fus_core_new().
 */
#define _GNU_SOURCE
#include "core.h"
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------ state ------------------------------------ */

typedef struct {                 /* radar mirror slot (latest frame) */
    FusRadTgt t;
    double az, el, r;            /* derived at ingest, RAW (trim applied at use) */
    double rdot;                 /* dr/dt, m/s (negative = closing) */
    double omega;                /* d(az)/dt, rad/s */
} RSlot;

typedef struct { int used; uint32_t gid;
    int r_tid, e_tid;            /* -1 = unbound */
    int fused;                   /* 1 = pair confirmed */
    uint64_t fused_ns;           /* when the pair confirmed */
    double miss_s;               /* divorce accumulator (co-fresh disagreement) */
    uint64_t rs_seen, es_seen;   /* input seqs consumed by the last co-fresh eval */
    uint64_t cofresh_ns;         /* time of the last co-fresh eval */
    /* published-state memory (re-bind gate + divorce arbitration) */
    double az, el, vaz, vel;
    /* range state (radar-fed) */
    int have_r; double r_f, rdot_f; uint64_t r_ns; int r_rejects;
    /* class vote (fusion-owned label) */
    double votes[4]; int cls; uint64_t votes_ns;
    /* liveness / re-bind */
    uint64_t r_lost_ns, e_lost_ns;   /* 0 = live; else when the tid left the wire */
    int rb_r_tid, rb_r_cnt;
    int rb_e_tid, rb_e_cnt;
    uint64_t born_ns, last_live_ns;
} FTrk;

typedef struct { int used; int r_tid, e_tid;
    uint32_t hist; int n;            /* co-fresh eval window (bit 0 = newest) */
    uint64_t rs_seen, es_seen, last_hit_ns;
} Cand;

typedef struct { int r_tid, e_tid; uint64_t until_ns; } Bar;

struct FusCore {
    FusKnobs k;
    /* stores */
    RSlot    rad[FUS_MAX_RAD]; int nrad; uint64_t rad_ns; uint64_t rad_seq;
    FusEoTrk eo[FUS_MAX_EO];   int neo;  uint64_t eo_ns;  uint64_t eo_seq;
    /* EO geometry */
    int img_w, img_h; double ifov;
    /* tables */
    FTrk trk[FUS_MAX_TRK];
    Cand cand[FUS_MAX_CAND];
    Bar  bar[16]; int nbar;
    uint32_t next_gid;
    uint64_t reset_until_ns[2];      /* [0]=radar, [1]=EO: sigma x2 grace */
    /* trim residual estimator (observe-only) */
    double est_az[FUS_TRIM_EST_RING], est_el[FUS_TRIM_EST_RING];
    int est_n, est_head;
    /* counters for /stats */
    int n_fused, n_eo_only, n_rad_only;
};

/* ------------------------------ helpers ---------------------------------- */

static double clampd(double v, double lo, double hi){ return v<lo?lo:v>hi?hi:v; }
static double sq(double v){ return v*v; }

/* Signed seconds since `a`. The two sensor wires are stamped by different
 * pipelines with different latencies, so the tick time is NOT monotonic
 * across sensors - a naive uint64 subtraction underflows on a backwards
 * step and made "22 ms ago" look like centuries (the radar-flicker bug,
 * REC 20260722T165848Z). Negative = `a` is newer than t; durations clamp
 * that to 0 at the call site. */
static double since_s(uint64_t t, uint64_t a)
{
    return (double)(int64_t)(t - a) / 1e9;
}

FusCore *fus_core_new(void)
{
    FusCore *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);   /* pre-fault: first-frame latency must match steady state */
    c->k.trim_az = FUS_TRIM_AZ_DEG_DEFAULT * M_PI / 180.0;
    c->k.trim_el = FUS_TRIM_EL_DEG_DEFAULT * M_PI / 180.0;
    c->k.gate = FUS_GATE_SCALE_DEFAULT;
    c->k.confirm = FUS_CONFIRM_DEFAULT;
    c->k.divorce_s = FUS_DIVORCE_S_DEFAULT;
    c->k.coast_s = FUS_COAST_S_DEFAULT;
    c->img_w = EO_IMG_W_DEFAULT; c->img_h = EO_IMG_H_DEFAULT;
    c->ifov = EO_IFOV_RAD_DEFAULT;
    c->next_gid = 1;
    for (int i = 0; i < FUS_MAX_TRK; i++) { c->trk[i].r_tid = c->trk[i].e_tid = -1; }
    return c;
}
void fus_core_free(FusCore *c){ free(c); }
void fus_core_set_knobs(FusCore *c, const FusKnobs *k){ c->k = *k; }
void fus_core_get_knobs(const FusCore *c, FusKnobs *k){ *k = c->k; }
void fus_core_set_eo_geom(FusCore *c, int w, int h, double ifov)
{
    if (w > 0) c->img_w = w;
    if (h > 0) c->img_h = h;
    if (ifov > 0) c->ifov = ifov;
}

static RSlot *rad_by_tid(FusCore *c, int tid)
{
    if (tid < 0) return NULL;
    for (int i = 0; i < c->nrad; i++) if (c->rad[i].t.tid == tid) return &c->rad[i];
    return NULL;
}
static FusEoTrk *eo_by_tid(FusCore *c, int tid)
{
    if (tid < 0) return NULL;
    for (int i = 0; i < c->neo; i++) if (c->eo[i].tid == tid) return &c->eo[i];
    return NULL;
}
static int rad_fresh(const FusCore *c, uint64_t t)
{ return c->rad_ns && since_s(t, c->rad_ns) < FUS_RAD_FRESH_S; }
static int eo_fresh(const FusCore *c, uint64_t t)
{ return c->eo_ns && since_s(t, c->eo_ns) < FUS_EO_FRESH_S; }

/* rig-frame radar angles (trim applied) */
static double r_az(const FusCore *c, const RSlot *s){ return s->az + c->k.trim_az; }
static double r_el(const FusCore *c, const RSlot *s){ return s->el + c->k.trim_el; }

static int in_eo_fov(const FusCore *c, double az, double el)
{
    double hw = 0.5 * c->img_w * c->ifov - FUS_FOV_MARGIN_RAD;
    double hh = 0.5 * c->img_h * c->ifov - FUS_FOV_MARGIN_RAD;
    return fabs(az) < hw && fabs(el) < hh;
}

static FTrk *trk_by_rtid(FusCore *c, int tid)
{
    if (tid < 0) return NULL;
    for (int i = 0; i < FUS_MAX_TRK; i++)
        if (c->trk[i].used && c->trk[i].r_tid == tid) return &c->trk[i];
    return NULL;
}
static FTrk *trk_by_etid(FusCore *c, int tid)
{
    if (tid < 0) return NULL;
    for (int i = 0; i < FUS_MAX_TRK; i++)
        if (c->trk[i].used && c->trk[i].e_tid == tid) return &c->trk[i];
    return NULL;
}
static FTrk *trk_alloc(FusCore *c, uint64_t t)
{
    for (int i = 0; i < FUS_MAX_TRK; i++) if (!c->trk[i].used) {
        FTrk *f = &c->trk[i];
        memset(f, 0, sizeof *f);
        f->used = 1; f->gid = c->next_gid++;
        f->r_tid = f->e_tid = -1;
        f->rb_r_tid = f->rb_e_tid = -1;
        f->born_ns = f->last_live_ns = t;
        return f;
    }
    return NULL;
}

static int barred(const FusCore *c, int r_tid, int e_tid, uint64_t t)
{
    for (int i = 0; i < c->nbar; i++)
        if (c->bar[i].r_tid == r_tid && c->bar[i].e_tid == e_tid &&
            t < c->bar[i].until_ns) return 1;
    return 0;
}
static void bar_add(FusCore *c, int r_tid, int e_tid, uint64_t t)
{
    int slot = 0;
    if (c->nbar < 16) slot = c->nbar++;
    else { uint64_t best = UINT64_MAX;               /* overwrite the oldest */
           for (int i = 0; i < 16; i++) if (c->bar[i].until_ns < best) { best = c->bar[i].until_ns; slot = i; } }
    c->bar[slot].r_tid = r_tid; c->bar[slot].e_tid = e_tid;
    c->bar[slot].until_ns = t + (uint64_t)(FUS_REPAIR_BAR_S * 1e9);
}

/* --------------------- association distance ------------------------------ */

/* D^2 between a radar slot and an EO track, both taken as-published (the wires
 * are already smoothed; prediction is only used for staleness inflation).
 * Returns D^2 and the chi-square gate for the number of terms used. */
static double pair_d2(const FusCore *c, const RSlot *r, const FusEoTrk *e,
                      uint64_t t, double *gate_out)
{
    double infl = 1.0;                     /* sigma scale during a reset grace */
    if (t < c->reset_until_ns[0] || t < c->reset_until_ns[1]) infl = 2.0;

    double dt_r = c->rad_ns ? clampd(since_s(t, c->rad_ns), 0, FUS_MAX_EXTRAP_S) : 0;
    double dt_e = c->eo_ns  ? clampd(since_s(t, c->eo_ns),  0, FUS_MAX_EXTRAP_S) : 0;

    double s_eaz = fmax(e->s_az, FUS_EO_SIG_FLOOR), s_eel = fmax(e->s_el, FUS_EO_SIG_FLOOR);
    double S_az = sq(infl) * (sq(FUS_SIG_RAD_AZ) + sq(s_eaz))
                + sq(FUS_EXTRAP_INFL * r->omega * dt_r) + sq(FUS_EXTRAP_INFL * e->vaz * dt_e);
    double S_el = sq(infl) * (sq(FUS_SIG_RAD_EL) + sq(s_eel));

    double d_az = (r_az(c, r) + r->omega * dt_r) - (e->az + e->vaz * dt_e);
    double d_el = r_el(c, r) - (e->el + e->vel * dt_e);

    double d2 = sq(d_az) / S_az;
    int terms = 1;

    /* elevation: only when neither side moves fast vertically (climbing rule) */
    if (fabs(e->vel) < FUS_EL_RATE_DROP) { d2 += sq(d_el) / S_el; terms++; }

    /* cross-range rate consistency */
    double S_w = sq(infl) * (sq(FUS_SIG_RAD_RDOT / fmax(r->r, 1.0)) + sq(FUS_SIG_EO_RATE));
    d2 += sq(r->omega - e->vaz) / S_w; terms++;

    /* looming consistency: radar closure predicts EO box growth */
    double g_pred = fmax(0.0, -r->rdot) / fmax(r->r, 1.0);
    if (r->t.mv == 1 && g_pred > FUS_GROW_ARM_GPRED && e->age_s > FUS_GROW_ARM_AGE_S) {
        d2 += sq(g_pred - e->grow) / sq(FUS_SIG_GROW); terms++;
    }

    double gate = terms >= 4 ? FUS_CHI2_4 : terms == 3 ? FUS_CHI2_3 : FUS_CHI2_2;
    *gate_out = gate * sq(c->k.gate);
    return d2;
}

/* --------------------------- pass stages ---------------------------------- */

static void update_liveness(FusCore *c, uint64_t t)
{
    int rlive = rad_fresh(c, t), elive = eo_fresh(c, t);
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        FTrk *f = &c->trk[i]; if (!f->used) continue;
        if (f->r_tid >= 0) {
            if (rlive && rad_by_tid(c, f->r_tid)) { f->r_lost_ns = 0; f->last_live_ns = t; }
            else if (!f->r_lost_ns) f->r_lost_ns = t;
        }
        if (f->e_tid >= 0) {
            if (elive && eo_by_tid(c, f->e_tid)) { f->e_lost_ns = 0; f->last_live_ns = t; }
            else if (!f->e_lost_ns) f->e_lost_ns = t;
        }
    }
}

/* Is this live tid claimed by any row (bound) or being courted for re-bind? */
static int rad_tid_claimed(const FusCore *c, int tid)
{
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        const FTrk *f = &c->trk[i]; if (!f->used) continue;
        if (f->r_tid == tid && !f->r_lost_ns) return 1;
        if (f->rb_r_tid == tid && f->rb_r_cnt > 0) return 1;
    }
    return 0;
}
static int eo_tid_claimed(const FusCore *c, int tid)
{
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        const FTrk *f = &c->trk[i]; if (!f->used) continue;
        if (f->e_tid == tid && !f->e_lost_ns) return 1;
        if (f->rb_e_tid == tid && f->rb_e_cnt > 0) return 1;
    }
    return 0;
}

/* Re-bind: a row whose bound tid left the wire adopts a new tid that sits in
 * its continuity gate for FUS_REBIND_FRAMES consecutive frames of that sensor.
 * Called only from the step of the sensor that just produced a frame. */
static void rebind_scan_rad(FusCore *c, uint64_t t)
{
    if (!rad_fresh(c, t)) return;
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        FTrk *f = &c->trk[i]; if (!f->used) continue;
        if (f->r_tid < 0 || !f->r_lost_ns) { f->rb_r_tid = -1; f->rb_r_cnt = 0; continue; }
        double dt = clampd(since_s(t, f->r_lost_ns), 0, FUS_REBIND_GRACE_S);
        double paz = f->az + f->vaz * dt, pel = f->el + f->vel * dt;
        double pr  = f->have_r ? f->r_f + f->rdot_f * dt : -1;
        double gaz = FUS_REBIND_SIGMA_K * FUS_SIG_RAD_AZ;
        double gel = FUS_REBIND_SIGMA_K * FUS_SIG_RAD_EL;
        int hit = -1;
        for (int j = 0; j < c->nrad; j++) {
            RSlot *s = &c->rad[j];
            if (trk_by_rtid(c, s->t.tid) && !trk_by_rtid(c, s->t.tid)->r_lost_ns) continue;
            if (fabs(r_az(c, s) - paz) > gaz || fabs(r_el(c, s) - pel) > gel) continue;
            if (pr > 0 && fabs(s->r - pr) > FUS_REBIND_RANGE_M + fabs(f->rdot_f) * dt) continue;
            hit = s->t.tid; break;
        }
        if (hit < 0) { f->rb_r_tid = -1; f->rb_r_cnt = 0; continue; }
        if (hit == f->rb_r_tid) f->rb_r_cnt++;
        else { f->rb_r_tid = hit; f->rb_r_cnt = 1; }
        if (f->rb_r_cnt >= FUS_REBIND_FRAMES) {
            /* the new tid may have grown its own young row - absorb it */
            FTrk *o = trk_by_rtid(c, hit);
            if (o && o != f) { o->used = 0; }
            f->r_tid = hit; f->r_lost_ns = 0; f->rb_r_tid = -1; f->rb_r_cnt = 0;
            f->last_live_ns = t;
        }
    }
}
static void rebind_scan_eo(FusCore *c, uint64_t t)
{
    if (!eo_fresh(c, t)) return;
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        FTrk *f = &c->trk[i]; if (!f->used) continue;
        if (f->e_tid < 0 || !f->e_lost_ns) { f->rb_e_tid = -1; f->rb_e_cnt = 0; continue; }
        double dt = clampd(since_s(t, f->e_lost_ns), 0, FUS_REBIND_GRACE_S);
        double paz = f->az + f->vaz * dt, pel = f->el + f->vel * dt;
        int hit = -1;
        for (int j = 0; j < c->neo; j++) {
            FusEoTrk *e = &c->eo[j];
            FTrk *o = trk_by_etid(c, e->tid);
            if (o && !o->e_lost_ns) continue;
            double g = FUS_REBIND_SIGMA_K * fmax(e->s_az, FUS_EO_SIG_FLOOR) + 0.5 * e->aw;
            if (fabs(e->az - paz) > g || fabs(e->el - pel) > g) continue;
            hit = e->tid; break;
        }
        if (hit < 0) { f->rb_e_tid = -1; f->rb_e_cnt = 0; continue; }
        if (hit == f->rb_e_tid) f->rb_e_cnt++;
        else { f->rb_e_tid = hit; f->rb_e_cnt = 1; }
        if (f->rb_e_cnt >= FUS_REBIND_FRAMES) {
            FTrk *o = trk_by_etid(c, hit);
            if (o && o != f) { o->used = 0; }
            f->e_tid = hit; f->e_lost_ns = 0; f->rb_e_tid = -1; f->rb_e_cnt = 0;
            f->last_live_ns = t;
        }
    }
}

/* Every live tid gets a row (a gid) unless it is a re-bind candidate. */
static void ensure_rows(FusCore *c, uint64_t t)
{
    if (rad_fresh(c, t))
        for (int i = 0; i < c->nrad; i++) {
            int tid = c->rad[i].t.tid;
            if (rad_tid_claimed(c, tid)) continue;
            FTrk *f = trk_alloc(c, t);
            if (!f) return;
            f->r_tid = tid;
            f->az = r_az(c, &c->rad[i]); f->el = r_el(c, &c->rad[i]);
            f->vaz = c->rad[i].omega; f->vel = 0;
            f->have_r = 1; f->r_f = c->rad[i].r; f->rdot_f = c->rad[i].rdot; f->r_ns = t;
        }
    if (eo_fresh(c, t))
        for (int i = 0; i < c->neo; i++) {
            int tid = c->eo[i].tid;
            if (eo_tid_claimed(c, tid)) continue;
            FTrk *f = trk_alloc(c, t);
            if (!f) return;
            f->e_tid = tid;
            f->az = c->eo[i].az; f->el = c->eo[i].el;
            f->vaz = c->eo[i].vaz; f->vel = c->eo[i].vel;
        }
}

/* Sustain fused pairs: hold or divorce. A co-fresh eval consumes one new frame
 * from EACH sensor, so the eval rate is min(rad_fps, eo_fps). */
static void sustain_fused(FusCore *c, uint64_t t)
{
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        FTrk *f = &c->trk[i];
        if (!f->used || !f->fused) continue;
        RSlot *r = (f->r_lost_ns == 0) ? rad_by_tid(c, f->r_tid) : NULL;
        FusEoTrk *e = (f->e_lost_ns == 0) ? eo_by_tid(c, f->e_tid) : NULL;
        if (!r || !e) continue;                          /* one-sided: degrade, no divorce */
        if (c->rad_seq <= f->rs_seen || c->eo_seq <= f->es_seen) continue;
        if (e->coast_s > FUS_EO_COFRESH_S) continue;     /* EO coasting: not evidence */
        if (!in_eo_fov(c, r_az(c, r), r_el(c, r))) {     /* FOV edge: freeze, not evidence */
            f->rs_seen = c->rad_seq; f->es_seen = c->eo_seq; f->cofresh_ns = t; continue;
        }
        double gate, d2 = pair_d2(c, r, e, t, &gate);
        double dt = f->cofresh_ns ? clampd(since_s(t, f->cofresh_ns), 0, 0.5) : 0;
        f->rs_seen = c->rad_seq; f->es_seen = c->eo_seq; f->cofresh_ns = t;
        if (d2 < gate * FUS_GATE_INCUMBENT) {
            f->miss_s = 0;
            /* healthy co-observation: feed the observe-only trim estimator */
            if (r->t.mv == 1 && e->state == 1 && e->coast_s == 0) {
                c->est_az[c->est_head] = e->az - r->az;   /* the trim that would zero it */
                c->est_el[c->est_head] = e->el - r->el;
                c->est_head = (c->est_head + 1) % FUS_TRIM_EST_RING;
                if (c->est_n < FUS_TRIM_EST_RING) c->est_n++;
            }
        } else {
            f->miss_s += dt;
            if (f->miss_s > c->k.divorce_s) {
                /* divorce: the gid follows the side closer to the row's last
                 * published angles; the other side gets a fresh row. */
                double de = sq((e->az - f->az) / fmax(e->s_az, FUS_EO_SIG_FLOOR));
                double dr = sq((r_az(c, r) - f->az) / FUS_SIG_RAD_AZ);
                bar_add(c, f->r_tid, f->e_tid, t);
                FTrk *n = trk_alloc(c, t);
                if (de <= dr) {          /* EO keeps the gid */
                    if (n) { n->r_tid = f->r_tid;
                             n->az = r_az(c, r); n->el = r_el(c, r);
                             n->vaz = r->omega; n->vel = 0;
                             n->have_r = 1; n->r_f = r->r; n->rdot_f = r->rdot; n->r_ns = t; }
                    f->r_tid = -1; f->r_lost_ns = 0; f->have_r = 0;
                } else {                 /* radar keeps the gid */
                    if (n) { n->e_tid = f->e_tid;
                             n->az = e->az; n->el = e->el; n->vaz = e->vaz; n->vel = e->vel; }
                    f->e_tid = -1; f->e_lost_ns = 0;
                    memset(f->votes, 0, sizeof f->votes); f->cls = 0;
                }
                f->fused = 0; f->miss_s = 0;
            }
        }
    }
}

static Cand *cand_find(FusCore *c, int r_tid, int e_tid)
{
    for (int i = 0; i < FUS_MAX_CAND; i++)
        if (c->cand[i].used && c->cand[i].r_tid == r_tid && c->cand[i].e_tid == e_tid)
            return &c->cand[i];
    return NULL;
}

static void promote(FusCore *c, Cand *cd, uint64_t t)
{
    FTrk *fe = trk_by_etid(c, cd->e_tid);
    FTrk *fr = trk_by_rtid(c, cd->r_tid);
    if (!fe || !fr || fe == fr) { cd->used = 0; return; }
    /* the older row keeps its gid; the younger is absorbed */
    FTrk *keep = (fe->born_ns <= fr->born_ns) ? fe : fr;
    FTrk *drop = (keep == fe) ? fr : fe;
    keep->r_tid = fr->r_tid; keep->e_tid = fe->e_tid;
    keep->r_lost_ns = fr->r_lost_ns; keep->e_lost_ns = fe->e_lost_ns;
    keep->have_r = fr->have_r; keep->r_f = fr->r_f; keep->rdot_f = fr->rdot_f;
    keep->r_ns = fr->r_ns; keep->r_rejects = 0;
    if (keep != fe) { memcpy(keep->votes, fe->votes, sizeof keep->votes);
                      keep->cls = fe->cls; keep->votes_ns = fe->votes_ns; }
    keep->fused = 1; keep->fused_ns = t; keep->miss_s = 0;
    keep->rs_seen = c->rad_seq; keep->es_seen = c->eo_seq; keep->cofresh_ns = t;
    keep->last_live_ns = t;
    drop->used = 0;
    cd->used = 0;
}

/* Candidate formation + confirmation. Greedy over in-gate edges, ascending
 * D^2 with a stickiness bonus for edges that already have a candidate. */
static void candidates(FusCore *c, uint64_t t)
{
    /* age out candidates whose tids left the wire */
    for (int i = 0; i < FUS_MAX_CAND; i++) {
        Cand *cd = &c->cand[i]; if (!cd->used) continue;
        FTrk *fe = trk_by_etid(c, cd->e_tid), *fr = trk_by_rtid(c, cd->r_tid);
        if ((fe && fe->fused) || (fr && fr->fused) ||
            (cd->last_hit_ns && since_s(t, cd->last_hit_ns) > 1.0))
            cd->used = 0;
    }
    if (!rad_fresh(c, t) || !eo_fresh(c, t)) return;

    typedef struct { double d2; int ri, ei; } Edge;
    Edge edge[FUS_MAX_RAD * 4];
    int ne = 0;
    for (int ri = 0; ri < c->nrad && ne < (int)(sizeof edge / sizeof edge[0]); ri++) {
        RSlot *r = &c->rad[ri];
        FTrk *fr = trk_by_rtid(c, r->t.tid);
        if (fr && fr->fused) continue;
        if (r->t.sus || r->t.conf < FUS_MIN_RAD_CONF) continue;
        if (!in_eo_fov(c, r_az(c, r), r_el(c, r))) continue;
        for (int ei = 0; ei < c->neo && ne < (int)(sizeof edge / sizeof edge[0]); ei++) {
            FusEoTrk *e = &c->eo[ei];
            FTrk *fe = trk_by_etid(c, e->tid);
            if (fe && fe->fused) continue;
            if (e->state != 1 || e->coast_s > FUS_EO_COFRESH_S) continue;
            if (barred(c, r->t.tid, e->tid, t)) continue;
            double gate, d2 = pair_d2(c, r, e, t, &gate);
            if (d2 >= gate) continue;
            if (cand_find(c, r->t.tid, e->tid)) d2 -= FUS_STICKY_BONUS;
            edge[ne].d2 = d2; edge[ne].ri = ri; edge[ne].ei = ei; ne++;
        }
    }
    /* insertion sort ascending (ne is small) */
    for (int i = 1; i < ne; i++) {
        int j = i; while (j > 0 && edge[j].d2 < edge[j-1].d2) {
            Edge tmp = edge[j];
            edge[j] = edge[j-1]; edge[j-1] = tmp; j--;
        }
    }
    int rused[FUS_MAX_RAD] = {0}, eused[FUS_MAX_EO] = {0};
    for (int i = 0; i < ne; i++) {
        if (rused[edge[i].ri] || eused[edge[i].ei]) continue;
        rused[edge[i].ri] = 1; eused[edge[i].ei] = 1;
        int rt = c->rad[edge[i].ri].t.tid, et = c->eo[edge[i].ei].tid;
        Cand *cd = cand_find(c, rt, et);
        if (!cd) {
            for (int k2 = 0; k2 < FUS_MAX_CAND; k2++) if (!c->cand[k2].used) { cd = &c->cand[k2]; break; }
            if (!cd) continue;
            memset(cd, 0, sizeof *cd);
            cd->used = 1; cd->r_tid = rt; cd->e_tid = et;
            cd->rs_seen = c->rad_seq - 1; cd->es_seen = c->eo_seq - 1;
        }
        if (c->rad_seq > cd->rs_seen && c->eo_seq > cd->es_seen) {
            cd->hist = (cd->hist << 1) | 1;
            cd->n++; cd->last_hit_ns = t;
            cd->rs_seen = c->rad_seq; cd->es_seen = c->eo_seq;
        }
    }
    /* advance a miss for candidates that had a co-fresh chance but no winning edge */
    for (int i = 0; i < FUS_MAX_CAND; i++) {
        Cand *cd = &c->cand[i]; if (!cd->used) continue;
        if (c->rad_seq > cd->rs_seen && c->eo_seq > cd->es_seen &&
            rad_by_tid(c, cd->r_tid) && eo_by_tid(c, cd->e_tid)) {
            cd->hist = (cd->hist << 1);
            cd->n++;
            cd->rs_seen = c->rad_seq; cd->es_seen = c->eo_seq;
        }
    }
    /* confirm: confirm+1 hits within the last 2*confirm evals */
    int M = c->k.confirm + 1, W = 2 * c->k.confirm;
    if (W > 30) W = 30;
    for (int i = 0; i < FUS_MAX_CAND; i++) {
        Cand *cd = &c->cand[i]; if (!cd->used || cd->n < M) continue;
        int hits = 0;
        for (int b = 0; b < W; b++) hits += (cd->hist >> b) & 1;
        if (hits >= M) promote(c, cd, t);
        else if (cd->n >= W && hits < M / 2) cd->used = 0;
    }
}

static void kill_dead(FusCore *c, uint64_t t)
{
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        FTrk *f = &c->trk[i]; if (!f->used) continue;
        int r_live = (f->r_tid >= 0 && !f->r_lost_ns);
        int e_live = (f->e_tid >= 0 && !f->e_lost_ns);
        if (r_live || e_live) continue;
        if (since_s(t, f->last_live_ns) > FUS_REBIND_GRACE_S) f->used = 0;
    }
}

/* ------------------------------ compose ----------------------------------- */

static void vote_class(FTrk *f, const FusEoTrk *e, uint64_t t)
{
    double dt = f->votes_ns ? fmax(0, since_s(t, f->votes_ns)) : 0;
    double decay = exp(-dt / FUS_CLS_DECAY_TAU);
    for (int k2 = 0; k2 < 4; k2++) f->votes[k2] *= decay;
    f->votes_ns = t;
    if (e && e->coast_s == 0 && e->cls >= 0 && e->cls < 4)
        f->votes[e->cls] += e->cls_conf > 0 ? e->cls_conf : 0.1;
    int best = f->cls;
    for (int k2 = 0; k2 < 4; k2++)
        if (f->votes[k2] > f->votes[best] * (k2 == f->cls ? 1.0 : FUS_CLS_SWITCH))
            best = k2;
    f->cls = best;
}

static int compose(FusCore *c, uint64_t t, FusOut *out, int max)
{
    int n = 0;
    c->n_fused = c->n_eo_only = c->n_rad_only = 0;
    for (int i = 0; i < FUS_MAX_TRK && n < max; i++) {
        FTrk *f = &c->trk[i]; if (!f->used) continue;
        RSlot *r = (f->r_tid >= 0 && !f->r_lost_ns) ? rad_by_tid(c, f->r_tid) : NULL;
        FusEoTrk *e = (f->e_tid >= 0 && !f->e_lost_ns) ? eo_by_tid(c, f->e_tid) : NULL;
        if (!r && !e) continue;                     /* re-bind grace holder: not published */

        FusOut *o = &out[n];
        memset(o, 0, sizeof *o);
        o->gid = f->gid;
        o->eo_tid = -1; o->rad_tid = -1;
        o->r_m = -1; o->rdot_mps = 0;
        o->eo_coast_s = -1; o->rad_coast_s = -1;
        o->eo_hits = -1; o->rad_np = -1; o->sus = -1; o->mv = -1;

        /* class vote runs whenever the EO side is live */
        if (e) vote_class(f, e, t);

        /* angles: EO owns them while fresh enough; else radar (trimmed) */
        if (e && e->coast_s < FUS_EO_ANGLE_MAX_COAST) {
            o->az = e->az; o->el = e->el; o->aw = e->aw; o->ah = e->ah;
            o->vaz = e->vaz; o->vel = e->vel; o->ang_src = 1;
        } else if (r) {
            o->az = r_az(c, r); o->el = r_el(c, r);
            o->aw = 2.0 * r->t.sx / fmax(r->r, 1.0);
            o->ah = 2.0 * r->t.sz / fmax(r->r, 1.0);
            o->vaz = r->omega; o->vel = 0; o->ang_src = 0;
        } else if (e) {                              /* EO deep in coast, still the only side */
            o->az = e->az; o->el = e->el; o->aw = e->aw; o->ah = e->ah;
            o->vaz = e->vaz; o->vel = e->vel; o->ang_src = 1;
        }

        /* range state */
        if (r) {
            double dt = f->r_ns ? fmax(0, since_s(t, f->r_ns)) : 0;
            double pred = f->have_r ? f->r_f + f->rdot_f * dt : r->r;
            double lim = fmax(FUS_R_CLAMP_BASE, 3.0 * FUS_R_CLAMP_BIN + fabs(f->rdot_f) * dt);
            if (!f->have_r || fabs(r->r - pred) <= lim || f->r_rejects >= 1) {
                f->r_f = r->r; f->r_rejects = 0;
                f->rdot_f += FUS_RDOT_EMA * (r->rdot - f->rdot_f);
            } else { f->r_f = pred; f->r_rejects++; }
            f->have_r = 1; f->r_ns = t;
            o->r_m = f->r_f; o->rdot_mps = f->rdot_f; o->r_stale = 0;
        } else if (f->have_r && f->r_lost_ns) {
            double lost = fmax(0, since_s(t, f->r_lost_ns));
            if (lost < FUS_R_DROP_S) {
                double dt = clampd(lost, 0, FUS_R_PROP_S);
                o->r_m = f->r_f + f->rdot_f * dt;
                o->rdot_mps = f->rdot_f;
                o->r_stale = lost > FUS_R_STALE_S;
            } else f->have_r = 0;
        }

        /* per-side passthrough */
        double conf_e = 0, conf_r = 0;
        if (e) {
            o->eo_tid = f->e_tid; o->eo_coast_s = e->coast_s;
            o->eo_hits = e->hits; o->grow = e->grow;
            o->lock_on = e->lock_on; o->lock_score = e->lock_score;
            conf_e = e->conf * clampd(1.0 - e->coast_s / c->k.coast_s, 0, 1);
        }
        if (r) {
            o->rad_tid = f->r_tid;
            o->rad_coast_s = fmax(0, since_s(t, c->rad_ns));
            o->rad_np = r->t.np; o->sus = r->t.sus; o->mv = r->t.mv;
            conf_r = r->t.conf;
        } else if (f->r_tid >= 0 && f->r_lost_ns && o->r_m > 0) {
            /* radar side lost but range still credited: show the dead binding */
            o->rad_tid = f->r_tid;
            o->rad_coast_s = fmax(0, since_s(t, f->r_lost_ns));
        }

        o->src = (f->fused && r && e) ? FUS_SRC_FUSED : (e ? FUS_SRC_EO : FUS_SRC_RAD);
        o->fused_age_s = f->fused ? fmax(0, since_s(t, f->fused_ns)) : 0;
        o->cls = f->cls;
        double vsum = f->votes[0] + f->votes[1] + f->votes[2] + f->votes[3];
        o->cls_conf = vsum > 0 ? f->votes[f->cls] / vsum : 0;
        o->conf = (r && e) ? fmin(conf_e, conf_r) : (e ? conf_e : conf_r);

        /* refresh the row's angle memory (re-bind gate reference) */
        f->az = o->az; f->el = o->el; f->vaz = o->vaz; f->vel = o->vel;

        if (o->src == FUS_SRC_FUSED) c->n_fused++;
        else if (o->src == FUS_SRC_EO) c->n_eo_only++;
        else c->n_rad_only++;
        n++;
    }
    return n;
}

/* ------------------------------ steps ------------------------------------- */

static int pass(FusCore *c, uint64_t t, FusOut *out, int max)
{
    update_liveness(c, t);
    ensure_rows(c, t);
    sustain_fused(c, t);
    candidates(c, t);
    kill_dead(c, t);
    return compose(c, t, out, max);
}

int fus_core_step_rad(FusCore *c, const FusRadTgt *t_in, int n, uint64_t t,
                      FusOut *out, int max)
{
    if (n > FUS_MAX_RAD) n = FUS_MAX_RAD;
    for (int i = 0; i < n; i++) {
        RSlot *s = &c->rad[i];
        s->t = t_in[i];
        double x = s->t.x, y = s->t.y, z = s->t.z;
        double rxy2 = x * x + y * y;
        s->r = sqrt(rxy2 + z * z);
        s->az = atan2(x, y);
        s->el = atan2(z, sqrt(rxy2));
        s->rdot = s->r > 1e-6 ? (x * s->t.vx + y * s->t.vy + z * s->t.vz) / s->r : 0;
        s->omega = rxy2 > 1e-6 ? (y * s->t.vx - x * s->t.vy) / rxy2 : 0;
    }
    c->nrad = n; c->rad_ns = t; c->rad_seq++;
    update_liveness(c, t);
    rebind_scan_rad(c, t);
    return pass(c, t, out, max);
}

int fus_core_step_eo(FusCore *c, const FusEoTrk *t_in, int n, uint64_t t,
                     FusOut *out, int max)
{
    if (n > FUS_MAX_EO) n = FUS_MAX_EO;
    memcpy(c->eo, t_in, (size_t)n * sizeof *t_in);
    c->neo = n; c->eo_ns = t; c->eo_seq++;
    update_liveness(c, t);
    rebind_scan_eo(c, t);
    return pass(c, t, out, max);
}

int fus_core_tick(FusCore *c, uint64_t t, FusOut *out, int max)
{
    return pass(c, t, out, max);
}

void fus_core_sensor_reset(FusCore *c, int sensor, uint64_t t)
{
    c->reset_until_ns[sensor ? 1 : 0] = t + (uint64_t)(FUS_RESET_GRACE_S * 1e9);
    for (int i = 0; i < FUS_MAX_TRK; i++) {
        FTrk *f = &c->trk[i]; if (!f->used) continue;
        if (!sensor && f->r_tid >= 0 && !f->r_lost_ns) f->r_lost_ns = t;
        if (sensor  && f->e_tid >= 0 && !f->e_lost_ns) f->e_lost_ns = t;
    }
    if (!sensor) { c->nrad = 0; c->rad_ns = 0; }
    else         { c->neo = 0;  c->eo_ns = 0; }
}

void fus_core_counts(const FusCore *c, int *fused, int *eo_only, int *rad_only)
{
    *fused = c->n_fused; *eo_only = c->n_eo_only; *rad_only = c->n_rad_only;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
int fus_core_trim_est(const FusCore *c, double *est_az, double *est_el)
{
    if (c->est_n == 0) { *est_az = *est_el = 0; return 0; }
    static double tmp[FUS_TRIM_EST_RING];
    memcpy(tmp, c->est_az, (size_t)c->est_n * sizeof *tmp);
    qsort(tmp, (size_t)c->est_n, sizeof *tmp, cmp_d);
    *est_az = tmp[c->est_n / 2];
    memcpy(tmp, c->est_el, (size_t)c->est_n * sizeof *tmp);
    qsort(tmp, (size_t)c->est_n, sizeof *tmp, cmp_d);
    *est_el = tmp[c->est_n / 2];
    return c->est_n;
}
