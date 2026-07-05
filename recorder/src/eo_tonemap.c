/* eo_tonemap — canonical Y10 -> 8-bit tone map + median. See eo_tonemap.h.
 * Extracted verbatim from the live ISP so the EO feed and native replay share
 * one implementation. State and scratch are caller-owned (thread-safe: the
 * recorder renders concurrent replays; the EO feed keeps its own statics). */
#include "eo_tonemap.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* gamma LUT: 8-bit -> 8-bit, v^(1/gamma). Depends only on the compile-time
 * constant, so compute-once with a benign first-call race (same values). */
static uint8_t g_lut[256];
static int     g_lut_ready = 0;
static void lut_init(void)
{
    for (int i = 0; i < 256; i++) {
        double v = pow(i / 255.0, 1.0 / EO_TONE_GAMMA) * 255.0;
        g_lut[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    g_lut_ready = 1;
}

void eo_tonemap(const uint8_t *y10, int bpl, int cx, int cy, int cw, int ch,
                uint8_t *out8, int ow, int oh, EoToneState *st,
                uint16_t *sm, int *xs)
{
    if (!g_lut_ready) lut_init();
    int npx = ow * oh;
    if (!sm || !xs) return;

    /* crop + box-average downscale (precompute the column map once per frame:
     * the naive per-pixel integer divide is a real frame-budget overrun) */
    for (int ox = 0; ox <= ow; ox++) xs[ox] = cx + ox * cw / ow;
    for (int oy = 0; oy < oh; oy++) {
        int sy0 = cy + oy * ch / oh, sy1 = cy + (oy + 1) * ch / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
        uint16_t *orow = sm + (size_t)oy * ow;
        for (int ox = 0; ox < ow; ox++) {
            int sx0 = xs[ox], sx1 = xs[ox + 1]; if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned acc = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = y10 + (size_t)sy * bpl;
                for (int sx = sx0; sx < sx1; sx++) { acc += (row[2*sx] | (row[2*sx+1] << 8)) >> 6; cnt++; }
            }
            orow[ox] = (uint16_t)(cnt ? acc / cnt : 0);
        }
    }

    /* p1/p99 percentile endpoints on the raw 10-bit, subsampled; EMA-smoothed
     * across frames (anti-breathing); span floored (no noise blow-up). */
    int hist[1024] = {0}, nh = 0;
    for (int i = 0; i < npx; i += 4) { hist[sm[i]]++; nh++; }
    int lo_t = (int)(nh * 0.01), hi_t = (int)(nh * 0.99);
    int a = 0, p_lo = 0, p_hi = 1023, got_lo = 0;
    for (int v = 0; v < 1024; v++) {
        a += hist[v];
        if (!got_lo && a >= lo_t) { p_lo = v; got_lo = 1; }
        if (a >= hi_t) { p_hi = v; break; }
    }
    if (!st->seeded) { st->s_lo = p_lo; st->s_hi = p_hi; st->seeded = 1; }
    st->s_lo = 0.85 * st->s_lo + 0.15 * p_lo;
    st->s_hi = 0.85 * st->s_hi + 0.15 * p_hi;
    double lo = st->s_lo, hi = st->s_hi;
    if (hi - lo < EO_TONE_MIN_SPAN) hi = lo + EO_TONE_MIN_SPAN;
    double scale = 255.0 / (hi - lo);

    /* per-frame 1024-entry LUT folds stretch + gamma into one lookup/pixel */
    uint8_t map[1024];
    for (int v = 0; v < 1024; v++) {
        double s = (v - lo) * scale;
        int q = (int)(s < 0 ? 0 : s > 255 ? 255 : s);
        map[v] = g_lut[q];
    }
    for (int i = 0; i < npx; i++) out8[i] = map[sm[i]];
}

/* Drift alarm: tone-map a fixed synthetic frame, hash the 8-bit output.
 * Deterministic (fresh EMA state, no median), so the hash is stable for a given
 * algorithm and changes the moment the math does. */
uint32_t eo_tonemap_hash(void)
{
    enum { W = 64, H = 48, N = W * H };
    uint8_t y16[N * 2], out[N];
    uint16_t sm[N];
    int xs[W + 1];
    for (int i = 0; i < N; i++) {           /* diagonal ramp + a bright corner */
        int v = ((i * 37) & 0x3ff);
        if ((i % W) < 4 && (i / W) < 4) v = 1000;
        uint16_t le = (uint16_t)(v << 6);
        y16[2 * i] = (uint8_t)le; y16[2 * i + 1] = (uint8_t)(le >> 8);
    }
    EoToneState st = { 0, 0, 0 };
    eo_tonemap(y16, W * 2, 0, 0, W, H, out, W, H, &st, sm, xs);
    uint32_t h = 2166136261u;               /* FNV-1a */
    for (int i = 0; i < N; i++) { h ^= out[i]; h *= 16777619u; }
    return h;
}

/* ---- 3x3 median (Devillard's 9-element network); NEON on aarch64 ---- */
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define VSORT(a, b) { uint8x16_t t_ = vminq_u8(a, b); b = vmaxq_u8(a, b); a = t_; }
#endif
#define PIX_SWAP(a, b) { uint8_t t_ = (a); (a) = (b); (b) = t_; }
#define PIX_SORT(a, b) { if ((a) > (b)) PIX_SWAP((a), (b)); }

void eo_median3(uint8_t *img, int w, int h, uint8_t *src)
{
    if (w < 3 || h < 3 || !src) return;
    memcpy(src, img, (size_t)w * h);
    for (int y = 1; y < h - 1; y++) {
        const uint8_t *r0 = src + (size_t)(y - 1) * w;
        const uint8_t *r1 = src + (size_t)y * w;
        const uint8_t *r2 = src + (size_t)(y + 1) * w;
        uint8_t *o = img + (size_t)y * w;
        int x = 1;
#if defined(__aarch64__) || defined(__ARM_NEON)
        for (; x + 16 <= w - 1; x += 16) {
            uint8x16_t p0 = vld1q_u8(r0+x-1), p1 = vld1q_u8(r0+x), p2 = vld1q_u8(r0+x+1);
            uint8x16_t p3 = vld1q_u8(r1+x-1), p4 = vld1q_u8(r1+x), p5 = vld1q_u8(r1+x+1);
            uint8x16_t p6 = vld1q_u8(r2+x-1), p7 = vld1q_u8(r2+x), p8 = vld1q_u8(r2+x+1);
            VSORT(p1,p2); VSORT(p4,p5); VSORT(p7,p8);
            VSORT(p0,p1); VSORT(p3,p4); VSORT(p6,p7);
            VSORT(p1,p2); VSORT(p4,p5); VSORT(p7,p8);
            VSORT(p0,p3); VSORT(p5,p8); VSORT(p4,p7);
            VSORT(p3,p6); VSORT(p1,p4); VSORT(p2,p5);
            VSORT(p4,p7); VSORT(p4,p2); VSORT(p6,p4);
            VSORT(p4,p2);
            vst1q_u8(o + x, p4);
        }
#endif
        for (; x < w - 1; x++) {
            uint8_t p[9] = { r0[x-1], r0[x], r0[x+1],
                             r1[x-1], r1[x], r1[x+1],
                             r2[x-1], r2[x], r2[x+1] };
            PIX_SORT(p[1],p[2]); PIX_SORT(p[4],p[5]); PIX_SORT(p[7],p[8]);
            PIX_SORT(p[0],p[1]); PIX_SORT(p[3],p[4]); PIX_SORT(p[6],p[7]);
            PIX_SORT(p[1],p[2]); PIX_SORT(p[4],p[5]); PIX_SORT(p[7],p[8]);
            PIX_SORT(p[0],p[3]); PIX_SORT(p[5],p[8]); PIX_SORT(p[4],p[7]);
            PIX_SORT(p[3],p[6]); PIX_SORT(p[1],p[4]); PIX_SORT(p[2],p[5]);
            PIX_SORT(p[4],p[7]); PIX_SORT(p[4],p[2]); PIX_SORT(p[6],p[4]);
            PIX_SORT(p[4],p[2]);
            o[x] = p[4];
        }
    }
}
