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

static uint8_t g_lut[256];
static int     g_lut_ready = 0;
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
    uint16_t *sm = malloc((size_t)npx * sizeof(uint16_t));   /* downscaled 10-bit */
    if (!sm) return;
    for (int oy = 0; oy < oh; oy++) {
        int sy0 = cy + oy * ch / oh, sy1 = cy + (oy + 1) * ch / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int ox = 0; ox < ow; ox++) {
            int sx0 = cx + ox * cw / ow, sx1 = cx + (ox + 1) * cw / ow; if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned acc = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = y10 + (size_t)sy * bpl;
                for (int sx = sx0; sx < sx1; sx++) { acc += (row[2*sx] | (row[2*sx+1] << 8)) >> 6; cnt++; }
            }
            sm[oy * ow + ox] = (uint16_t)(cnt ? acc / cnt : 0);
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
    for (int i = 0; i < npx; i++) {
        double s = (sm[i] - lo) * scale;
        int q = (int)(s < 0 ? 0 : s > 255 ? 255 : s);
        out8[i] = g_lut[q];
    }
    free(sm);
}

/* 3x3 median (Devillard's optimal 9-element network), edge-preserving grain filter.
 * Reads a scratch copy so the in-place write can't corrupt a pixel's own neighbours;
 * the 1px border is left as-is. */
#define PIX_SWAP(a, b) { uint8_t t_ = (a); (a) = (b); (b) = t_; }
#define PIX_SORT(a, b) { if ((a) > (b)) PIX_SWAP((a), (b)); }
void isp_median3(uint8_t *img, int w, int h)
{
    if (w < 3 || h < 3) return;
    uint8_t *src = malloc((size_t)w * h);
    if (!src) return;
    memcpy(src, img, (size_t)w * h);
    for (int y = 1; y < h - 1; y++) {
        const uint8_t *r0 = src + (size_t)(y - 1) * w;
        const uint8_t *r1 = src + (size_t)y * w;
        const uint8_t *r2 = src + (size_t)(y + 1) * w;
        uint8_t *o = img + (size_t)y * w;
        for (int x = 1; x < w - 1; x++) {
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
    free(src);
}
