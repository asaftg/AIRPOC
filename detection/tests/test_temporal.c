/* test_temporal — unit tests for the evidence integrator (src/temporal.c).
 *
 *   make test && ./test_temporal
 *
 * temporal.c decides what the whole detector reports, it is pure deterministic C
 * with no dependencies, and its guarantees are quoted in the module README and the
 * I/O contract. Those guarantees are enforced here rather than trusted:
 *   1. a confident detection is reported on its first frame, unchanged
 *   2. one track reports at most one box per frame (no duplicate for one target)
 *   3. a faint target is reported after exactly the configured number of frames
 *   4. a faint target that has not earned it yet is not reported
 *   5. flicker never gets reported however long it goes on
 *   6. nothing is reported on a frame with no observation (no guessed positions)
 *   7. a reported faint target never claims less confidence than the confident tier
 *   8. straight-line movement grows `disp`; movement in place does not
 *   9. oversized inputs and a full track table are handled without corruption
 */
#include "temporal.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...) do {                                              \
    if (!(cond)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
                   printf(__VA_ARGS__); printf("\n"); g_fail++; }          \
} while (0)

static TbdParams P(void)
{
    TbdParams p = { .lo = DET_TBD_LO_DEFAULT, .hi = DET_CONF_DEFAULT,
                    .frames = DET_TBD_FRAMES_DEFAULT,
                    .miss_penalty = DET_TBD_DECAY_DEFAULT,
                    .max_miss = DET_TBD_MAXMISS_DEFAULT, .fps = 15.0 };
    return p;
}

static TbdIn mk(float cx, float cy, float conf)
{
    TbdIn d = { .cx = cx, .cy = cy, .w = 20, .h = 20, .conf = conf, .cls = "human" };
    return d;
}

/* 1 + 7: confident detections pass straight through, faint ones never undercut them */
static void test_confident_passthrough(void)
{
    printf("confident detection is reported immediately, unchanged\n");
    TbdCtx *c = tbd_new();
    TbdParams p = P();
    TbdIn in = mk(100, 100, 0.90f);
    TbdOut out[8];

    int n = tbd_process(c, &in, 1, &p, out, 8);
    CHECK(n == 1, "expected 1 box on the first frame, got %d", n);
    CHECK(out[0].tbd == 0, "a confident box must not be flagged as collected");
    CHECK(fabsf(out[0].conf - 0.90f) < 1e-6f,
          "confidence must be passed through unchanged, got %.4f", out[0].conf);
    CHECK(out[0].age == 0 && out[0].hits == 1, "age/hits wrong: %d/%d", out[0].age, out[0].hits);
    tbd_free(c);
}

/* 2: a target that is BOTH confident and long-established still yields one box */
static void test_no_duplicate(void)
{
    printf("one track never produces two boxes in a frame\n");
    TbdCtx *c = tbd_new();
    TbdParams p = P();
    TbdIn in = mk(100, 100, 0.90f);
    TbdOut out[8];

    for (int i = 0; i < 20; i++) {
        int n = tbd_process(c, &in, 1, &p, out, 8);
        CHECK(n == 1, "frame %d: expected exactly 1 box, got %d", i, n);
    }
    tbd_free(c);
}

/* 3 + 4: a faint target is reported after exactly `frames` frames, not before */
static void test_faint_needs_exact_frames(void)
{
    printf("faint target is reported after exactly the configured frame count\n");
    TbdCtx *c = tbd_new();
    TbdParams p = P();                       /* frames = 6, lo = 0.15 */
    TbdIn in = mk(200, 200, (float)p.lo);    /* sits exactly on the floor */
    TbdOut out[8];

    for (int i = 1; i < p.frames; i++) {
        int n = tbd_process(c, &in, 1, &p, out, 8);
        CHECK(n == 0, "frame %d of %d: nothing should be reported yet, got %d",
              i, p.frames, n);
    }
    int n = tbd_process(c, &in, 1, &p, out, 8);
    CHECK(n == 1, "frame %d: the target should now be reported, got %d", p.frames, n);
    CHECK(out[0].tbd == 1, "a promoted box must be flagged as collected");
    CHECK(out[0].conf >= (float)p.hi - 1e-6f,
          "a reported box must never sit below the confident tier (%.3f < %.3f)",
          out[0].conf, p.hi);
    CHECK(out[0].hits == p.frames, "hits should equal the frames seen, got %d", out[0].hits);
    tbd_free(c);
}

/* 5: something appearing intermittently must never be reported, however long it runs */
static void test_flicker_never_reported(void)
{
    printf("flicker is never reported, however long it flickers\n");
    TbdCtx *c = tbd_new();
    TbdParams p = P();
    TbdIn in = mk(300, 300, (float)p.lo);
    TbdOut out[8];

    for (int i = 0; i < 300; i++) {
        int n = (i % 3 == 0) ? tbd_process(c, &in, 1, &p, out, 8)
                             : tbd_process(c, NULL, 0, &p, out, 8);
        CHECK(n == 0, "frame %d: flicker must never be reported, got %d", i, n);
        if (g_fail) break;
    }
    tbd_free(c);
}

/* 6: once established, a frame with no detection reports nothing — never a guess */
static void test_no_coasted_box(void)
{
    printf("a frame with no detection reports nothing (no guessed positions)\n");
    TbdCtx *c = tbd_new();
    TbdParams p = P();
    TbdIn in = mk(400, 400, 0.90f);
    TbdOut out[8];

    for (int i = 0; i < 10; i++) tbd_process(c, &in, 1, &p, out, 8);   /* well established */
    int n = tbd_process(c, NULL, 0, &p, out, 8);
    CHECK(n == 0, "a frame with no detection must report nothing, got %d", n);
    tbd_free(c);
}

/* 8: the translate-vs-in-place discriminator the tracker is told it can rely on */
static void test_disp_translate_vs_in_place(void)
{
    printf("straight-line movement grows disp; movement in place does not\n");
    TbdOut out[8];
    TbdParams p = P();

    TbdCtx *a = tbd_new();
    float last_disp = 0;
    for (int i = 0; i < 12; i++) {                 /* travels steadily right */
        TbdIn in = mk(100.0f + 8.0f * i, 100, 0.90f);
        int n = tbd_process(a, &in, 1, &p, out, 8);
        CHECK(n == 1, "travelling target lost at frame %d", i);
        if (n == 1) last_disp = out[0].net_disp;
        if (g_fail) break;
    }
    CHECK(last_disp > 70.0f, "a target that travelled ~88 px should show it, got %.1f", last_disp);
    tbd_free(a);

    TbdCtx *b = tbd_new();
    float osc_disp = 0, osc_path = 0;
    for (int i = 0; i < 12; i++) {                 /* jiggles about one spot */
        TbdIn in = mk(100.0f + (i % 2 ? 10.0f : 0.0f), 100, 0.90f);
        int n = tbd_process(b, &in, 1, &p, out, 8);
        CHECK(n == 1, "in-place target lost at frame %d", i);
        if (n == 1) { osc_disp = out[0].net_disp; osc_path = out[0].path_len; }
        if (g_fail) break;
    }
    CHECK(osc_disp < 15.0f, "in-place movement must not look like travel, got disp %.1f", osc_disp);
    CHECK(osc_path > 3.0f * osc_disp,
          "path (%.1f) should far exceed net displacement (%.1f) for in-place movement",
          osc_path, osc_disp);
    tbd_free(b);
}

/* 9: hostile inputs — more candidates than the cap, and a saturated track table */
static void test_bounds(void)
{
    printf("oversized input and a full track table are handled safely\n");
    TbdCtx *c = tbd_new();
    TbdParams p = P();
    static TbdIn in[DET_TBD_MAX_IN + 64];
    static TbdOut out[DET_TBD_MAX_IN + 64];

    /* Spread far enough apart that none of them associate with each other. */
    for (int i = 0; i < DET_TBD_MAX_IN + 64; i++)
        in[i] = mk((float)(i % 200) * 300.0f, (float)(i / 200) * 300.0f, 0.90f);

    int n = tbd_process(c, in, DET_TBD_MAX_IN + 64, &p, out, DET_TBD_MAX_IN + 64);
    CHECK(n >= 0 && n <= DET_TBD_MAX_TRACKS,
          "output must be bounded by the track table, got %d", n);
    CHECK(tbd_live_tracks(c) <= DET_TBD_MAX_TRACKS,
          "track table overflowed: %d", tbd_live_tracks(c));

    /* A small output buffer must be respected. */
    n = tbd_process(c, in, 32, &p, out, 4);
    CHECK(n <= 4, "output buffer limit ignored: %d > 4", n);

    /* Degenerate parameters must not fault. */
    TbdParams z = P();
    z.frames = 0; z.fps = 0;
    tbd_process(c, in, 8, &z, out, 8);
    tbd_reset(c);
    CHECK(tbd_live_tracks(c) == 0, "reset left %d tracks", tbd_live_tracks(c));
    tbd_free(c);
}

int main(void)
{
    printf("test_temporal — evidence integrator\n\n");
    test_confident_passthrough();
    test_no_duplicate();
    test_faint_needs_exact_frames();
    test_flicker_never_reported();
    test_no_coasted_box();
    test_disp_translate_vs_in_place();
    test_bounds();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
