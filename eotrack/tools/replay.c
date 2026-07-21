/* replay.c - deterministic offline validation of the tracker core.
 *
 * Drives trk_core directly (no HTTP/tap/camera) through scripted detection streams
 * and asserts the invariants the radar tracker's bench also enforces: emitted tracks
 * are a subset of live tracks, negative scenes emit exactly zero, a translating target
 * confirms and emits, an in-place oscillator is latched off, a looming/classified
 * target is kept, coasting holds then drops, and an engaged tentative track is emitted.
 *
 * Exits nonzero on any failed assertion so it can gate a deploy. This is the seed of
 * the recorded-corpus bench (feed real det_wire replays through the same core).
 */
#include "core.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                              else printf("ok:   %s\n", msg); } while (0)

static const double DT = 1.0 / 15.0;

/* Run n ticks feeding a single detection produced by gen(tick, &det); returns the
 * last step's emitted count and fills last_out[0..]. */
typedef int (*Gen)(int tick, TrkDet *d);   /* returns #dets (0 or 1) */

static int run(TrkCore *c, Gen gen, int nticks, int engage, TrkOut *out, int *live_out)
{
    int emitted = 0;
    uint64_t ts = 1000;
    for (int k = 0; k < nticks; k++) {
        TrkDet d; int nd = gen(k, &d);
        ts += (uint64_t)(DT * 1e9);
        emitted = trk_core_step(c, nd ? &d : NULL, nd, ts, DT, 0, 0, engage, out, TRK_MAX_TRACKS);
    }
    int live, em; trk_core_counts(c, &live, &em);
    if (live_out) *live_out = live;
    return emitted;
}

static int gen_translate(int k, TrkDet *d)   /* human moving right 6 px/tick */
{
    d->src = 0; d->cls = 1; d->conf = 0.8; d->tbd = 0; d->hits = -1; d->age = -1;
    d->cx = 300 + 6.0 * k; d->cy = 500; d->w = 40; d->h = 80;
    return 1;
}
static int gen_oscillate(int k, TrkDet *d)   /* class-less mover bouncing in place */
{
    d->src = 1; d->cls = 0; d->conf = 0.5; d->tbd = 0; d->hits = -1; d->age = -1;
    d->cx = 700 + 8.0 * ((k % 2) ? 1 : -1); d->cy = 400; d->w = 10; d->h = 10;
    return 1;
}
static int gen_loom(int k, TrkDet *d)        /* class-less mover, stationary, growing */
{
    d->src = 1; d->cls = 0; d->conf = 0.6; d->tbd = 0; d->hits = -1; d->age = -1;
    d->cx = 720; d->cy = 540; d->w = 8 + 0.8 * k; d->h = 8 + 0.8 * k;
    return 1;
}
static int gen_static_human(int k, TrkDet *d) /* classified, parked (no motion) */
{
    (void)k;
    d->src = 0; d->cls = 1; d->conf = 0.7; d->tbd = 0; d->hits = -1; d->age = -1;
    d->cx = 200; d->cy = 300; d->w = 30; d->h = 70;
    return 1;
}
static int gen_none(int k, TrkDet *d) { (void)k; (void)d; return 0; }

/* weak single-hit blips at random-ish places (should never confirm/emit) */
static int gen_blip(int k, TrkDet *d)
{
    d->src = 1; d->cls = 0; d->conf = 0.3; d->tbd = 0; d->hits = -1; d->age = -1;
    d->cx = 100 + (k * 137 % 1200); d->cy = 100 + (k * 91 % 900); d->w = 6; d->h = 6;
    return 1;
}

int main(void)
{
    TrkOut out[TRK_MAX_TRACKS];
    int live;

    /* 1. translating human -> confirmed + emitted, latch on, stable tid */
    { TrkCore *c = trk_core_new();
      int em = run(c, gen_translate, 40, -1, out, &live);
      CHECK(em >= 1, "translating target is emitted");
      CHECK(!strcmp(out[0].state, "conf"), "translating target confirmed");
      CHECK(out[0].vx > 10, "velocity captured (px/s, moving right)");
      trk_core_free(c); }

    /* 2. in-place oscillator -> confirmed internally but latched OFF (not emitted) */
    { TrkCore *c = trk_core_new();
      int em = run(c, gen_oscillate, 60, -1, out, &live);
      CHECK(live >= 1, "oscillator is tracked internally");
      CHECK(em == 0, "in-place oscillator is latched off (E subset of live)");
      trk_core_free(c); }

    /* 3. empty scene -> zero emitted */
    { TrkCore *c = trk_core_new();
      int em = run(c, gen_none, 30, -1, out, &live);
      CHECK(em == 0 && live == 0, "empty scene emits nothing");
      trk_core_free(c); }

    /* 4. looming radial approacher (net ~0 displacement) rescued by size growth */
    { TrkCore *c = trk_core_new();
      int em = run(c, gen_loom, 40, -1, out, &live);
      CHECK(em >= 1, "looming (growing) target is kept despite ~0 net displacement");
      trk_core_free(c); }

    /* 5. classified but parked human -> kept (class rescues the translate test) */
    { TrkCore *c = trk_core_new();
      int em = run(c, gen_static_human, 40, -1, out, &live);
      CHECK(em >= 1, "classified parked target is kept");
      CHECK(out[0].cls == 1, "class is human");
      trk_core_free(c); }

    /* 6. coast: target vanishes -> emits coast briefly then drops */
    { TrkCore *c = trk_core_new();
      /* build to confirmed */
      uint64_t ts = 1000; int em = 0;
      for (int k = 0; k < 30; k++) { TrkDet d; gen_translate(k, &d); ts += (uint64_t)(DT*1e9);
          em = trk_core_step(c, &d, 1, ts, DT, 0, 0, -1, out, TRK_MAX_TRACKS); }
      CHECK(em >= 1, "target confirmed before loss");
      /* one missed tick: should still emit as coast */
      ts += (uint64_t)(DT*1e9);
      em = trk_core_step(c, NULL, 0, ts, DT, 0, 0, -1, out, TRK_MAX_TRACKS);
      CHECK(em >= 1 && !strcmp(out[0].state, "coast"), "coasts on a missed tick");
      /* many missed ticks: should drop */
      for (int k = 0; k < 30; k++) { ts += (uint64_t)(DT*1e9);
          em = trk_core_step(c, NULL, 0, ts, DT, 0, 0, -1, out, TRK_MAX_TRACKS); }
      trk_core_counts(c, &live, &em);
      CHECK(live == 0, "coasting track eventually dies");
      trk_core_free(c); }

    /* 7. engage: an unconfirmed engaged track is emitted immediately */
    { TrkCore *c = trk_core_new();
      TrkDet d; gen_translate(0, &d);
      uint64_t ts = 1000;
      int em = trk_core_step(c, &d, 1, ts, DT, 0, 0, -1, out, TRK_MAX_TRACKS);
      CHECK(em == 0, "single-hit track not emitted before engage");
      int tid = -1;
      /* find its tid via engaged_box scan: engage tid 1 (first allocated) */
      double cx,cy,w,h; int cls;
      CHECK(trk_core_engaged_box(c, 1, &cx,&cy,&w,&h,&cls), "track 1 exists");
      tid = 1;
      ts += (uint64_t)(DT*1e9); gen_translate(1, &d);
      em = trk_core_step(c, &d, 1, ts, DT, 0, 0, tid, out, TRK_MAX_TRACKS);
      CHECK(em >= 1 && out[0].tid == tid, "engaged track emitted while still tentative");
      trk_core_free(c); }

    /* 8. blip storm negative: transient single-frame junk never confirms/emits */
    { TrkCore *c = trk_core_new();
      int em = run(c, gen_blip, 80, -1, out, &live);
      CHECK(em == 0, "moving single-frame blips never emit");
      trk_core_free(c); }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
