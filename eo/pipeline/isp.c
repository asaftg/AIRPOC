/* Light mono ISP for the wire feed. Two jobs:
 *   - metrics on the NATIVE frame: AE mean (isp_mean10) + focus sharpness (isp_sharpness)
 *   - isp_scale_tonemap: crop(zoom) + black-level/adaptive-white tone map + gamma in one
 *     pass. Output is native resolution (ow=cw, oh=ch): no downscale, full detail. It
 *     supports box-average downscale if ever asked (ow<cw), but the feed runs 1:1.
 *     The detector uses the raw Y10; tone/gamma here is for the human view only. */
#include "pipeline.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static uint8_t g_lut[256];
static int     g_lut_ready = 0;
static volatile int g_destripe = 1;   /* row-noise correction on by default */
void isp_set_destripe(int on) { g_destripe = on ? 1 : 0; }
int  isp_destripe_on(void)    { return g_destripe; }
static void lut_init(void)
{
    for (int i = 0; i < 256; i++) {
        double v = pow(i / 255.0, 1.0 / EO_GAMMA) * 255.0;
        g_lut[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    g_lut_ready = 1;
}

static inline int y10_at(const uint8_t *y10, int bpl, int x, int y)
{
    const uint8_t *p = y10 + (size_t)y * bpl + (size_t)x * 2;
    return (p[0] | (p[1] << 8)) >> 6;          /* 0..1023 */
}

/* Mean of an 8x8 subsample, 10-bit scale — the AE metric. */
double isp_mean10(const uint8_t *y10, int bpl, int w, int h)
{
    uint64_t sum = 0; int n = 0;
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8) { sum += y10_at(y10, bpl, x, y); n++; }
    return n ? (double)sum / n : 0.0;
}

/* Focus-assist sharpness (Tenengrad) over the native center ROI, on the 10-bit
 * values — no tone-map needed. Higher = sharper; the UI shows it as % of peak. */
double isp_sharpness(const uint8_t *y10, int bpl, int w, int h)
{
    int x0 = w * 3 / 10, x1 = w * 7 / 10, y0 = h * 3 / 10, y1 = h * 7 / 10;
    uint64_t sum = 0; int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int gx = y10_at(y10, bpl, x + 1, y) - y10_at(y10, bpl, x - 1, y);
            int gy = y10_at(y10, bpl, x, y + 1) - y10_at(y10, bpl, x, y - 1);
            sum += (uint64_t)(gx * gx + gy * gy); n++;
        }
    return n ? (double)sum / n : 0.0;
}

/* Crop (cx,cy,cw,ch) of the Y10 frame, box-average downscale to ow*oh, then
 * black-level + adaptive-white tone map + gamma -> 8-bit. The crop is the digital
 * zoom; box-average anti-aliases the downscale. One pass, small output. */
void isp_scale_tonemap(const uint8_t *y10, int bpl, int cx, int cy, int cw, int ch,
                       uint8_t *out8, int ow, int oh)
{
    if (!g_lut_ready) lut_init();
    int npx = ow * oh;
    /* hot path (60 fps): cached work buffers, no per-frame malloc; the column map is
     * precomputed once per frame — the naive form costs a per-PIXEL integer division
     * (~1.5M/frame at native), which was exactly the producer's frame-budget overrun. */
    static uint16_t *sm = NULL;  static int sm_cap = 0;
    static int      *xs = NULL;  static int xs_cap = 0;
    if (npx > sm_cap) { free(sm); sm = malloc((size_t)npx * sizeof(uint16_t)); sm_cap = sm ? npx : 0; }
    if (ow + 1 > xs_cap) { free(xs); xs = malloc((size_t)(ow + 1) * sizeof(int)); xs_cap = xs ? ow + 1 : 0; }
    if (!sm || !xs) return;
    for (int ox = 0; ox <= ow; ox++) xs[ox] = cx + ox * cw / ow;   /* once, not per pixel */
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
    /* Row-noise correction (destripe) — remove each row's fixed/temporal offset BEFORE
     * the night stretch amplifies it into horizontal lines. The IMX296's row noise is
     * only ~1 LSB, but a dark scene (signal a few LSB above black) gets stretched ~6x,
     * turning that 1 LSB into visible banding. Each row's offset = the high-frequency
     * part of its median (a boxcar low-pass over rows preserves the real vertical scene
     * gradient); subtract it. Cost: one 10-bit histogram-median per row + a boxcar + one
     * subtract per pixel — a few M ops/frame, negligible vs the encode. */
    if (g_destripe && oh >= 8) {
        static float *rmed = NULL; static int rmed_cap = 0;
        if (oh > rmed_cap) { free(rmed); rmed = malloc((size_t)oh * sizeof(float)); rmed_cap = rmed ? oh : 0; }
        if (rmed) {
            /* per-row median from a column-subsampled histogram (every 4th px — the median
             * is robust to it, and it cuts the histogram build 4x for the native case). */
            int nsamp = (ow + 3) / 4, half = nsamp / 2;
            for (int y = 0; y < oh; y++) {
                int rh[1024]; memset(rh, 0, sizeof rh);
                const uint16_t *r = sm + (size_t)y * ow;
                for (int x = 0; x < ow; x += 4) rh[r[x]]++;
                int acc = 0, med = 0;
                for (int v = 0; v < 1024; v++) { acc += rh[v]; if (acc >= half) { med = v; break; } }
                rmed[y] = (float)med;
            }
            const int R = 16;                           /* low-pass radius: keeps scene gradient */
            for (int y = 0; y < oh; y++) {
                int a0 = y - R, b0 = y + R; if (a0 < 0) a0 = 0; if (b0 >= oh) b0 = oh - 1;
                float s = 0; for (int k = a0; k <= b0; k++) s += rmed[k];
                /* clamp: a large row-median residual is real horizontal scene structure,
                 * not the ~1 LSB row FPN — never subtract it (that ghosts the scene). */
                int off = (int)lround(rmed[y] - s / (b0 - a0 + 1));
                if (off >  EO_DESTRIPE_MAX) off =  EO_DESTRIPE_MAX;
                if (off < -EO_DESTRIPE_MAX) off = -EO_DESTRIPE_MAX;
                if (!off) continue;
                uint16_t *r = sm + (size_t)y * ow;
                int x = 0;
#if defined(__aarch64__) || defined(__ARM_NEON)
                uint16x8_t vmax = vdupq_n_u16(1023);
                if (off > 0) { uint16x8_t vo = vdupq_n_u16((uint16_t)off);       /* saturating sub -> clamps at 0 */
                    for (; x + 8 <= ow; x += 8) vst1q_u16(r + x, vminq_u16(vqsubq_u16(vld1q_u16(r + x), vo), vmax)); }
                else { uint16x8_t vo = vdupq_n_u16((uint16_t)(-off));            /* saturating add -> clamps at max */
                    for (; x + 8 <= ow; x += 8) vst1q_u16(r + x, vminq_u16(vqaddq_u16(vld1q_u16(r + x), vo), vmax)); }
#endif
                for (; x < ow; x++) { int v = (int)r[x] - off; r[x] = (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v); }
            }
        }
    }
    /* p1/p99 percentile endpoints on the raw 10-bit. p99 (not max/99.5%) ignores a
     * small blown streetlight; p1 sets a real black. Endpoints are EMA-smoothed across
     * frames so the mapping doesn't wobble frame-to-frame (the "breathing"), and the
     * span is floored so a flat/dim scene isn't blown up to full range (6x noise gain).*/
    int hist[1024] = {0}, nh = 0;
    for (int i = 0; i < npx; i += 4) { hist[sm[i]]++; nh++; }   /* subsample: percentiles don't need every px */
    int lo_t = (int)(nh * 0.01), hi_t = (int)(nh * 0.99);
    int a = 0, p_lo = 0, p_hi = 1023, got_lo = 0;
    for (int v = 0; v < 1024; v++) {
        a += hist[v];
        if (!got_lo && a >= lo_t) { p_lo = v; got_lo = 1; }
        if (a >= hi_t) { p_hi = v; break; }
    }
    static double s_lo = -1.0, s_hi = -1.0;
    if (s_lo < 0.0) { s_lo = p_lo; s_hi = p_hi; }      /* seed on first frame */
    s_lo = 0.85 * s_lo + 0.15 * p_lo;
    s_hi = 0.85 * s_hi + 0.15 * p_hi;
    double lo = s_lo, hi = s_hi;
    if (hi - lo < EO_MIN_SPAN) hi = lo + EO_MIN_SPAN;
    double scale = 255.0 / (hi - lo);
    /* 10-bit -> 8-bit via a per-frame 1024-entry LUT (folds the stretch + gamma into
     * one table lookup per pixel instead of float math per pixel). */
    uint8_t map[1024];
    for (int v = 0; v < 1024; v++) {
        double s = (v - lo) * scale;
        int q = (int)(s < 0 ? 0 : s > 255 ? 255 : s);
        map[v] = g_lut[q];
    }
    for (int i = 0; i < npx; i++) out8[i] = map[sm[i]];
}

/* 3x3 median (Devillard's optimal 9-element sorting network), edge-preserving grain
 * filter. Reads a scratch copy so the in-place write can't corrupt a pixel's own
 * neighbours; the 1px border is left as-is.
 * On aarch64 the network runs in NEON — the same 19 min/max steps on 16 pixels per
 * instruction (~10x the scalar rate); the scalar tail covers the row remainder and
 * non-ARM builds (the x86 compile check). */
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define VSORT(a, b) { uint8x16_t t_ = vminq_u8(a, b); b = vmaxq_u8(a, b); a = t_; }
#endif
#define PIX_SWAP(a, b) { uint8_t t_ = (a); (a) = (b); (b) = t_; }
#define PIX_SORT(a, b) { if ((a) > (b)) PIX_SWAP((a), (b)); }
void isp_median3(uint8_t *img, int w, int h)
{
    if (w < 3 || h < 3) return;
    static uint8_t *src = NULL; static int cap = 0;      /* hot path: no per-frame malloc */
    if (w * h > cap) { free(src); src = malloc((size_t)w * h); cap = src ? w * h : 0; }
    if (!src) return;
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
