/* replay.c - offline validation gates for the fusion core (tier 1, synthetic).
 * Drives fus_core_step_rad/eo directly with interleaved 26/15 Hz frames from a
 * simulated world. Exits nonzero on the first failing gate. No I/O beyond
 * stdout; deterministic (LCG noise, simulated clock).
 *
 * Build: gcc -O3 -Wall -Wextra -std=c11 tools/replay.c src/core.c src/emit.c -o tools/replay -lm
 */
#define _GNU_SOURCE
#include "../src/core.h"
#include "../src/config.h"
#include "../src/emit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define D2R (M_PI / 180.0)

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
    g_fail = 1; return; } } while (0)

/* deterministic noise */
static unsigned long g_seed = 12345;
static double frand(void)
{
    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    return (double)((g_seed >> 33) & 0xffffffff) / 4294967296.0 - 0.5;
}

/* ------------------------------- world ----------------------------------- */

typedef struct {
    double az0, vaz;      /* rad, rad/s (truth, rig frame) */
    double el0, vel;
    double r0, rdot;      /* m, m/s */
    int    cls;           /* EO class */
    int    alive_rad, alive_eo;
    double eo_az_bias;    /* offset applied ONLY to EO (simulates disagreement) */
} Tgt;

typedef struct {
    Tgt    tgt[8]; int n;
    double mount_az, mount_el;   /* radar mounting offset: radar reads truth - mount */
    double t;                    /* sim seconds */
    int    rad_tid_base, eo_tid_base;
    double eo_age[8];
} World;

static void world_init(World *w, int n)
{
    memset(w, 0, sizeof *w);
    w->n = n;
    w->rad_tid_base = 1; w->eo_tid_base = 1;
    for (int i = 0; i < n; i++) { w->tgt[i].alive_rad = w->tgt[i].alive_eo = 1; w->tgt[i].cls = 2; }
}

static double taz(const Tgt *g, double t){ return g->az0 + g->vaz * t; }
static double tel(const Tgt *g, double t){ return g->el0 + g->vel * t; }
static double tr (const Tgt *g, double t){ return g->r0  + g->rdot * t; }

static int world_rad_frame(World *w, FusRadTgt *out)
{
    int n = 0;
    for (int i = 0; i < w->n; i++) {
        Tgt *g = &w->tgt[i]; if (!g->alive_rad) continue;
        double az = taz(g, w->t) - w->mount_az + frand() * 2 * 0.6 * D2R;
        double el = tel(g, w->t) - w->mount_el + frand() * 2 * 2.0 * D2R;
        double r  = tr(g, w->t) + frand() * 2 * 1.0;
        FusRadTgt *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->tid = w->rad_tid_base + i;
        o->x = r * cos(el) * sin(az);
        o->y = r * cos(el) * cos(az);
        o->z = r * sin(el);
        /* velocities consistent with (rdot, vaz): v = d/dt of the position */
        o->vx = g->rdot * sin(az) + r * cos(az) * g->vaz;
        o->vy = g->rdot * cos(az) - r * sin(az) * g->vaz;
        o->vz = 0;
        o->sx = 1.0; o->sy = 1.0; o->sz = 0.5;
        o->conf = 1.0; o->np = 30; o->sus = 0; o->mv = 1;
    }
    return n;
}

static int world_eo_frame(World *w, FusEoTrk *out)
{
    int n = 0;
    for (int i = 0; i < w->n; i++) {
        Tgt *g = &w->tgt[i]; if (!g->alive_eo) continue;
        FusEoTrk *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->tid = w->eo_tid_base + i;
        o->state = 1;
        o->cls = g->cls; o->cls_conf = 0.9; o->conf = 0.8;
        o->az = taz(g, w->t) + g->eo_az_bias + frand() * 2 * 0.0003;
        o->el = tel(g, w->t) + frand() * 2 * 0.0003;
        o->aw = 0.01; o->ah = 0.02;
        o->vaz = g->vaz; o->vel = g->vel;
        o->s_az = o->s_el = 0.001;
        o->grow = g->rdot < 0 ? -g->rdot / tr(g, w->t) : 0.0;
        o->coast_s = 0;
        o->age_s = w->eo_age[i];
        o->hits = (int)(w->eo_age[i] * 15) + 1;
    }
    return n;
}

/* Interleaved run: radar 26 Hz, EO 15 Hz, for dur seconds. Calls chk() (if
 * given) after every pass with the emitted rows. */
typedef void (*RowChk)(const FusOut *rows, int n, double t, void *u);

static uint64_t sim_ns(double t){ return (uint64_t)(t * 1e9) + 1000000000ull; }

static void run(World *w, FusCore *c, double dur, RowChk chk, void *u)
{
    FusRadTgt rf[FUS_MAX_RAD]; FusEoTrk ef[FUS_MAX_EO]; FusOut rows[FUS_MAX_OUT];
    double t_end = w->t + dur;
    double next_rad = w->t, next_eo = w->t + 0.001;
    while (w->t < t_end) {
        double tn = next_rad < next_eo ? next_rad : next_eo;
        w->t = tn;
        int n;
        if (tn == next_rad) {
            int nr = world_rad_frame(w, rf);
            n = fus_core_step_rad(c, rf, nr, sim_ns(tn), rows, FUS_MAX_OUT);
            next_rad += 1.0 / 26.0;
        } else {
            for (int i = 0; i < w->n; i++) if (w->tgt[i].alive_eo) w->eo_age[i] += 1.0 / 15.0;
            int ne = world_eo_frame(w, ef);
            n = fus_core_step_eo(c, ef, ne, sim_ns(tn), rows, FUS_MAX_OUT);
            next_eo += 1.0 / 15.0;
        }
        if (chk) chk(rows, n, w->t, u);
    }
}

/* Wire invariant: a constituent tid never appears twice, and never both as a
 * constituent of one row and standalone in another. Checked on EVERY pass. */
static void invariant(const FusOut *rows, int n, double t, void *u)
{
    (void)u;
    for (int i = 0; i < n && !g_fail; i++)
        for (int j = i + 1; j < n; j++) {
            if (rows[i].eo_tid >= 0 && rows[i].eo_tid == rows[j].eo_tid) {
                printf("FAIL invariant t=%.2f: eo_tid %d in two rows\n", t, rows[i].eo_tid);
                g_fail = 1; return;
            }
            if (rows[i].rad_tid >= 0 && rows[i].rad_tid == rows[j].rad_tid) {
                printf("FAIL invariant t=%.2f: rad_tid %d in two rows\n", t, rows[i].rad_tid);
                g_fail = 1; return;
            }
        }
}

static const FusOut *find_src(const FusOut *rows, int n, FusSrc s)
{
    for (int i = 0; i < n; i++) if (rows[i].src == s) return &rows[i];
    return NULL;
}

/* ------------------------------- gates ------------------------------------ */

/* 1+2: one target seen by both -> exactly one fused row, gid stable, invariant. */
static struct { uint32_t gid; int fused_seen; int rows_after_confirm_ok; double t_fused; } A;
static void chk_assoc(const FusOut *rows, int n, double t, void *u)
{
    invariant(rows, n, t, u); if (g_fail) return;
    const FusOut *f = find_src(rows, n, FUS_SRC_FUSED);
    if (f) {
        if (!A.fused_seen) { A.fused_seen = 1; A.gid = f->gid; A.t_fused = t; }
        else if (f->gid != A.gid) {
            printf("FAIL assoc t=%.2f: fused gid changed %u -> %u\n", t, A.gid, f->gid);
            g_fail = 1; return;
        }
        if (n != 1) { printf("FAIL assoc t=%.2f: %d rows alongside the fused row\n", t, n); g_fail = 1; }
    }
}
static void t_association(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = 2 * D2R, .el0 = 1 * D2R, .r0 = 200, .rdot = -5,
                      .cls = 2, .alive_rad = 1, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    memset(&A, 0, sizeof A);
    run(&w, c, 3.0, chk_assoc, NULL);
    CHECK(!g_fail, "invariant/gid");
    CHECK(A.fused_seen, "no fused row formed in 3 s");
    CHECK(A.t_fused < 1.0, "confirmation took %.2f s (> 1 s)", A.t_fused);
    fus_core_free(c);
    printf("PASS association (fused at t=%.2f s, gid %u stable)\n", A.t_fused, A.gid);
}

/* 3: a brief excursion must not split the pair; sustained separation must. */
static void t_divorce(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = 0, .el0 = 0, .r0 = 150, .rdot = 0, .cls = 2,
                      .alive_rad = 1, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    run(&w, c, 2.0, invariant, NULL);
    CHECK(!g_fail, "invariant");

    /* blink: EO alone jumps 5 deg away for 2 EO frames, then returns */
    FusRadTgt rf[8]; FusEoTrk ef[8]; FusOut rows[FUS_MAX_OUT];
    w.tgt[0].eo_az_bias = 5 * D2R;
    for (int i = 0; i < 2; i++) {
        w.t += 1.0 / 15.0;
        int nr = world_rad_frame(&w, rf);
        fus_core_step_rad(c, rf, nr, sim_ns(w.t - 0.001), rows, FUS_MAX_OUT);
        int ne = world_eo_frame(&w, ef);
        fus_core_step_eo(c, ef, ne, sim_ns(w.t), rows, FUS_MAX_OUT);
    }
    w.tgt[0].eo_az_bias = 0;
    int n = 0;
    { int nr = world_rad_frame(&w, rf);
      n = fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.02), rows, FUS_MAX_OUT); }
    CHECK(find_src(rows, n, FUS_SRC_FUSED) != NULL, "2-frame blink split the pair");

    /* sustained: EO alone diverges for > divorce_s -> split into two rows */
    uint32_t gid_before = rows[0].gid;
    w.tgt[0].eo_az_bias = 5 * D2R;
    double t0 = w.t; int split = 0; double t_split = 0;
    while (w.t < t0 + 3.0) {
        w.t += 1.0 / 30.0;
        int nr = world_rad_frame(&w, rf);
        fus_core_step_rad(c, rf, nr, sim_ns(w.t - 0.001), rows, FUS_MAX_OUT);
        int ne = world_eo_frame(&w, ef);
        n = fus_core_step_eo(c, ef, ne, sim_ns(w.t), rows, FUS_MAX_OUT);
        invariant(rows, n, w.t, NULL);
        if (g_fail) break;
        if (!find_src(rows, n, FUS_SRC_FUSED) && n == 2) { split = 1; t_split = w.t - t0; break; }
    }
    CHECK(!g_fail, "invariant during divorce");
    CHECK(split, "sustained separation never split the pair");
    CHECK(t_split > 0.3, "split too fast (%.2f s) - hysteresis missing", t_split);
    const FusOut *eo = find_src(rows, n, FUS_SRC_EO);
    CHECK(eo && eo->gid == gid_before, "EO side did not keep the gid on divorce");
    fus_core_free(c);
    printf("PASS divorce (blink held, split after %.2f s, gid stayed with EO)\n", t_split);
}

/* 4: an old EO-only track fusing with a young radar track keeps the EO gid. */
static void t_gid_inherit(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = -3 * D2R, .el0 = 0.5 * D2R, .r0 = 250, .rdot = -3,
                      .cls = 1, .alive_rad = 0, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    FusOut rows[FUS_MAX_OUT];
    run(&w, c, 2.0, invariant, NULL);
    CHECK(!g_fail, "invariant");
    /* capture the EO-only gid */
    FusEoTrk ef[8]; int ne = world_eo_frame(&w, ef);
    int n = fus_core_step_eo(c, ef, ne, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    CHECK(n == 1 && rows[0].src == FUS_SRC_EO, "expected one eo row");
    uint32_t eo_gid = rows[0].gid;
    w.tgt[0].alive_rad = 1;
    double t0 = w.t; int fused = 0;
    FusRadTgt rf[8];
    while (w.t < t0 + 2.0 && !fused) {
        w.t += 1.0 / 26.0;
        int nr = world_rad_frame(&w, rf);
        fus_core_step_rad(c, rf, nr, sim_ns(w.t), rows, FUS_MAX_OUT);
        ne = world_eo_frame(&w, ef);
        n = fus_core_step_eo(c, ef, ne, sim_ns(w.t + 0.001), rows, FUS_MAX_OUT);
        invariant(rows, n, w.t, NULL);
        if (g_fail) return;
        const FusOut *f = find_src(rows, n, FUS_SRC_FUSED);
        if (f) { fused = 1;
                 CHECK(f->gid == eo_gid, "fused gid %u != older EO gid %u", f->gid, eo_gid); }
    }
    CHECK(fused, "never fused after radar appeared");
    fus_core_free(c);
    printf("PASS gid inheritance (older EO gid %u survived fusing)\n", eo_gid);
}

/* 5: passthrough fidelity on a fused row. */
static void t_passthrough(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = 1 * D2R, .el0 = 0, .r0 = 300, .rdot = -8, .cls = 2,
                      .alive_rad = 1, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    run(&w, c, 3.0, invariant, NULL);
    CHECK(!g_fail, "invariant");
    FusRadTgt rf[8]; FusOut rows[FUS_MAX_OUT];
    int nr = world_rad_frame(&w, rf);
    int n = fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    const FusOut *f = find_src(rows, n, FUS_SRC_FUSED);
    CHECK(f, "no fused row");
    CHECK(f->cls == 2, "class not vehicle (%d)", f->cls);
    CHECK(f->rdot_mps < -5, "closing target rdot %.1f not negative", f->rdot_mps);
    CHECK(fabs(f->r_m - tr(&w.tgt[0], w.t)) < 8, "range %.1f vs truth %.1f", f->r_m, tr(&w.tgt[0], w.t));
    CHECK(f->ang_src == 1, "fused angles not EO-sourced");
    CHECK(fabs(f->az - taz(&w.tgt[0], w.t)) < 0.002, "az off by %.4f", fabs(f->az - taz(&w.tgt[0], w.t)));
    CHECK(f->mv == 1 && f->sus == 0, "radar flags not passed through");
    CHECK(f->grow > 0.01, "grow not passed through");
    CHECK(f->eo_hits > 0 && f->rad_np > 0, "evidence counts missing");
    fus_core_free(c);
    printf("PASS passthrough (cls/r/rdot/ang/flags verified)\n");
}

/* 6: mount trim - a 3 deg az mount offset must block fusing until trimmed. */
static void t_trim(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = 0, .el0 = 0, .r0 = 200, .rdot = -4, .cls = 2,
                      .alive_rad = 1, .alive_eo = 1 };
    w.mount_az = 3 * D2R;
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k);
    k.trim_az = 0; k.trim_el = 0; fus_core_set_knobs(c, &k);
    memset(&A, 0, sizeof A);
    run(&w, c, 3.0, chk_assoc, NULL);
    CHECK(!g_fail, "invariant");
    CHECK(!A.fused_seen, "fused despite a 3 deg untrimmed mount offset");
    /* observe-only estimator has nothing (no fused pairs) - now set the trim */
    k.trim_az = 3 * D2R; fus_core_set_knobs(c, &k);
    memset(&A, 0, sizeof A);
    run(&w, c, 2.0, chk_assoc, NULL);
    CHECK(!g_fail, "invariant");
    CHECK(A.fused_seen, "did not fuse after trim was set");
    /* estimator now reports ~the mount offset (residual vs RAW radar angles) */
    double ea, ee; int cnt = fus_core_trim_est(c, &ea, &ee);
    CHECK(cnt > 10, "trim estimator empty (%d)", cnt);
    CHECK(fabs(ea - 3 * D2R) < 0.6 * D2R, "trim est %.2f deg vs 3.0 expected", ea / D2R);
    fus_core_free(c);
    printf("PASS trim (blocked untrimmed, fused trimmed, est %.2f deg n=%d)\n", ea / D2R, cnt);
}

/* 7: degrade - EO dies -> rad row same gid; radar dies -> eo row, range
 * propagates then goes stale then drops; both die -> empty heartbeat. */
static void t_degrade(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = 0.5 * D2R, .el0 = 0, .r0 = 180, .rdot = -6, .cls = 2,
                      .alive_rad = 1, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    run(&w, c, 3.0, invariant, NULL);
    CHECK(!g_fail, "invariant");
    FusRadTgt rf[8]; FusEoTrk ef[8]; FusOut rows[FUS_MAX_OUT];
    int nr = world_rad_frame(&w, rf);
    int n = fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    const FusOut *f = find_src(rows, n, FUS_SRC_FUSED);
    CHECK(f, "no fused row before degrade");
    uint32_t gid = f->gid;

    /* EO dies (tracker keeps running, target left its list) */
    w.tgt[0].alive_eo = 0;
    double t0 = w.t; const FusOut *rrow = NULL;
    while (w.t < t0 + 1.5) {
        w.t += 1.0 / 26.0;
        nr = world_rad_frame(&w, rf);
        n = fus_core_step_rad(c, rf, nr, sim_ns(w.t), rows, FUS_MAX_OUT);
        int ne = world_eo_frame(&w, ef);
        fus_core_step_eo(c, ef, ne, sim_ns(w.t + 0.001), rows, FUS_MAX_OUT);
        invariant(rows, n, w.t, NULL); if (g_fail) return;
    }
    nr = world_rad_frame(&w, rf);
    n = fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    rrow = find_src(rows, n, FUS_SRC_RAD);
    CHECK(rrow && rrow->gid == gid, "EO death did not degrade to rad row w/ same gid");
    CHECK(rrow->r_m > 0, "rad row lost its range");

    /* EO returns, radar dies -> eo row, range propagated then stale then gone */
    w.tgt[0].alive_eo = 1;
    t0 = w.t;
    while (w.t < t0 + 1.0) {          /* re-fuse first */
        w.t += 1.0 / 26.0;
        nr = world_rad_frame(&w, rf);
        fus_core_step_rad(c, rf, nr, sim_ns(w.t), rows, FUS_MAX_OUT);
        int ne = world_eo_frame(&w, ef);
        w.eo_age[0] += 1.0 / 26.0;
        n = fus_core_step_eo(c, ef, ne, sim_ns(w.t + 0.001), rows, FUS_MAX_OUT);
    }
    CHECK(find_src(rows, n, FUS_SRC_FUSED), "did not re-fuse after EO returned");
    gid = find_src(rows, n, FUS_SRC_FUSED)->gid;
    w.tgt[0].alive_rad = 0;
    /* radar daemon still frames (empty targets); EO continues */
    t0 = w.t; int saw_stale = 0, saw_drop = 0; double r_at_loss = 0;
    while (w.t < t0 + 4.0) {
        w.t += 1.0 / 26.0;
        nr = world_rad_frame(&w, rf);
        fus_core_step_rad(c, rf, nr, sim_ns(w.t), rows, FUS_MAX_OUT);
        int ne = world_eo_frame(&w, ef);
        w.eo_age[0] += 1.0 / 26.0;
        n = fus_core_step_eo(c, ef, ne, sim_ns(w.t + 0.001), rows, FUS_MAX_OUT);
        invariant(rows, n, w.t, NULL); if (g_fail) return;
        const FusOut *er = find_src(rows, n, FUS_SRC_EO);
        if (!er) continue;
        CHECK(er->gid == gid, "gid lost on radar death");
        if (er->r_m > 0 && !r_at_loss) r_at_loss = er->r_m;
        if (er->r_stale) saw_stale = 1;
        if (er->r_m < 0) { saw_drop = 1; break; }
    }
    CHECK(saw_stale, "range never flagged stale after radar loss");
    CHECK(saw_drop, "range never dropped after radar loss");

    /* both dead -> tick publishes nothing */
    w.tgt[0].alive_eo = 0;
    for (int i = 0; i < 40; i++) {
        w.t += 1.0 / 15.0;
        int ne = world_eo_frame(&w, ef);
        n = fus_core_step_eo(c, ef, ne, sim_ns(w.t), rows, FUS_MAX_OUT);
    }
    n = fus_core_tick(c, sim_ns(w.t += 5.0), rows, FUS_MAX_OUT);
    CHECK(n == 0, "%d rows still published with both sensors empty", n);
    fus_core_free(c);
    printf("PASS degrade (rad-hold gid, r stale->drop, empty when dark)\n");
}

/* 8: two crossing targets - constituent pairings must not swap. */
static void t_crossing(void)
{
    World w; world_init(&w, 2);
    w.tgt[0] = (Tgt){ .az0 = -4 * D2R, .vaz =  1.6 * D2R, .el0 = 0.3 * D2R,
                      .r0 = 150, .rdot = -2, .cls = 2, .alive_rad = 1, .alive_eo = 1 };
    w.tgt[1] = (Tgt){ .az0 =  4 * D2R, .vaz = -1.6 * D2R, .el0 = 0.3 * D2R,
                      .r0 = 220, .rdot = -2, .cls = 2, .alive_rad = 1, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    /* let both pairs confirm well before the crossing (cross at t=2.5) */
    run(&w, c, 1.5, invariant, NULL);
    CHECK(!g_fail, "invariant");
    FusRadTgt rf[8]; FusEoTrk ef[8]; FusOut rows[FUS_MAX_OUT];
    int nr = world_rad_frame(&w, rf);
    int n = fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    int pair_e[2] = {-1,-1};
    int npair = 0;
    for (int i = 0; i < n; i++) if (rows[i].src == FUS_SRC_FUSED) {
        if (rows[i].rad_tid == 1) pair_e[0] = rows[i].eo_tid;
        if (rows[i].rad_tid == 2) pair_e[1] = rows[i].eo_tid;
        npair++;
    }
    CHECK(npair == 2, "expected 2 fused pairs before crossing, got %d", npair);
    /* run through the crossing and 1.5 s beyond */
    run(&w, c, 4.0, invariant, NULL);
    CHECK(!g_fail, "invariant through crossing");
    nr = world_rad_frame(&w, rf);
    fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    int ne = world_eo_frame(&w, ef);
    n = fus_core_step_eo(c, ef, ne, sim_ns(w.t += 0.001), rows, FUS_MAX_OUT);
    for (int i = 0; i < n; i++) if (rows[i].src == FUS_SRC_FUSED) {
        if (rows[i].rad_tid == 1)
            CHECK(rows[i].eo_tid == pair_e[0], "pair swapped at crossing (rad 1 -> eo %d)", rows[i].eo_tid);
        if (rows[i].rad_tid == 2)
            CHECK(rows[i].eo_tid == pair_e[1], "pair swapped at crossing (rad 2 -> eo %d)", rows[i].eo_tid);
    }
    fus_core_free(c);
    printf("PASS crossing (2 pairs held constituents through the cross)\n");
}

/* 9: emit round-trip sanity + absent-side conventions. */
static void t_emit(void)
{
    World w; world_init(&w, 1);
    w.tgt[0] = (Tgt){ .az0 = 1 * D2R, .r0 = 100, .rdot = -3, .cls = 1,
                      .alive_rad = 1, .alive_eo = 1 };
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    FusOut rows[FUS_MAX_OUT];
    run(&w, c, 2.0, invariant, NULL);
    FusRadTgt rf[8];
    int nr = world_rad_frame(&w, rf);
    int n = fus_core_step_rad(c, rf, nr, sim_ns(w.t += 0.01), rows, FUS_MAX_OUT);
    static char buf[128 * 1024];
    FusHdr h = { .rad_connected = 1, .trk_connected = 1, .frame_id = 7,
                 .rad_frame_id = 100, .eo_frame_id = 50, .eo_engaged = -1,
                 .t_out_ns = sim_ns(w.t) };
    size_t len = fus_frame_json(buf, sizeof buf, &h, rows, n);
    CHECK(len > 0 && len < sizeof buf, "emit length %zu", len);
    CHECK(strstr(buf, "\"type\":\"fus\"") != NULL, "missing type");
    CHECK(strstr(buf, "\"src\":\"fus\"") != NULL, "missing fused row");
    CHECK(strstr(buf, "\"ang_src\":\"eo\"") != NULL, "missing ang_src");
    CHECK(strstr(buf, "nan") == NULL && strstr(buf, "inf") == NULL, "non-finite leaked");
    /* brace balance */
    int bal = 0; for (size_t i = 0; i < len; i++) { if (buf[i]=='{') bal++; if (buf[i]=='}') bal--; }
    CHECK(bal == 0, "unbalanced JSON (%d)", bal);
    fus_core_free(c);
    printf("PASS emit (well-formed, %zu bytes)\n", len);
}

/* 10: performance - worst-case pass under the budget. */
static void t_perf(void)
{
    FusCore *c = fus_core_new();
    FusKnobs k; fus_core_get_knobs(c, &k); k.trim_az = k.trim_el = 0; fus_core_set_knobs(c, &k);
    FusRadTgt rf[FUS_MAX_RAD]; FusEoTrk ef[FUS_MAX_EO]; FusOut rows[FUS_MAX_OUT];
    World w; world_init(&w, 8);
    for (int i = 0; i < 8; i++)
        w.tgt[i] = (Tgt){ .az0 = (i - 4) * 2 * D2R, .el0 = 0, .r0 = 100 + 40 * i,
                          .rdot = -3, .cls = 2, .alive_rad = 1, .alive_eo = 1 };
    /* build full worst-case stores: 64 radar x 64 EO spread over the FOV */
    for (int i = 0; i < FUS_MAX_RAD; i++) {
        memset(&rf[i], 0, sizeof rf[i]);
        rf[i].tid = i + 1;
        double az = ((i % 16) - 8) * 1.2 * D2R, r = 60 + 7 * i;
        rf[i].x = r * sin(az); rf[i].y = r * cos(az); rf[i].z = 2;
        rf[i].vx = 1; rf[i].vy = -2; rf[i].sx = rf[i].sy = 1; rf[i].sz = 0.5;
        rf[i].conf = 1; rf[i].np = 20; rf[i].mv = 1;
    }
    for (int i = 0; i < FUS_MAX_EO; i++) {
        memset(&ef[i], 0, sizeof ef[i]);
        ef[i].tid = i + 1; ef[i].state = 1; ef[i].cls = 2; ef[i].cls_conf = 0.9;
        ef[i].conf = 0.8;
        ef[i].az = ((i % 16) - 8) * 1.2 * D2R; ef[i].el = ((i / 16) - 2) * 2 * D2R;
        ef[i].aw = 0.01; ef[i].ah = 0.02; ef[i].s_az = ef[i].s_el = 0.001;
        ef[i].age_s = 5; ef[i].hits = 60;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    const int N = 2000;
    uint64_t t = 1000000000ull;
    for (int i = 0; i < N; i++) {
        t += 19000000ull;
        fus_core_step_rad(c, rf, FUS_MAX_RAD, t, rows, FUS_MAX_OUT);
        t += 19000000ull;
        fus_core_step_eo(c, ef, FUS_MAX_EO, t, rows, FUS_MAX_OUT);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3 / (2 * N);
    printf("perf: %.1f us/pass at 64x64 (budget 300)\n", us);
    CHECK(us < 300.0, "pass %.1f us exceeds the 300 us budget", us);
    fus_core_free(c);
    printf("PASS perf\n");
}

#define RUN(fn) do { g_fail = 0; fn(); if (g_fail) g_total = 1; } while (0)
static int g_total = 0;

int main(void)
{
    RUN(t_association);
    RUN(t_divorce);
    RUN(t_gid_inherit);
    RUN(t_passthrough);
    RUN(t_trim);
    RUN(t_degrade);
    RUN(t_crossing);
    RUN(t_emit);
    RUN(t_perf);
    if (g_total) { printf("REPLAY: FAIL\n"); return 1; }
    printf("REPLAY: ALL GATES PASS\n");
    return 0;
}
