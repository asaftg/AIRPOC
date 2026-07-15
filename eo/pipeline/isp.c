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

/* Spot AE metering for active illumination: the MEDIAN of the LIT pixels (above the
 * black level) inside a circular window on the beam. Median is robust to blown
 * retroreflectors (a road sign is the top tail, not the median) and skips the unlit
 * gaps inside the window; targeting it to EO_AE_TARGET exposes the illuminated SURFACE
 * to mid-gray instead of over-exposing it to lift a black-dominated whole-frame average.
 * Returns -1 when too little lit content is in the window (caller falls back to
 * whole-frame metering — the far/dark regime where spot can't help anyway). */
double isp_meter_spot(const uint8_t *y10, int bpl, int w, int h, int cx, int cy, int rad)
{
    int hist[1024]; memset(hist, 0, sizeof hist);
    long n = 0, r2 = (long)rad * rad;
    int lit = (int)EO_BLACK + 8;
    for (int y = cy - rad; y <= cy + rad; y += 2) {
        if (y < 0 || y >= h) continue;
        for (int x = cx - rad; x <= cx + rad; x += 2) {
            if (x < 0 || x >= w) continue;
            long dx = x - cx, dy = y - cy;
            if (dx*dx + dy*dy > r2) continue;
            int v = y10_at(y10, bpl, x, y);
            if (v > lit) { hist[v]++; n++; }
        }
    }
    if (n < 64) return -1.0;                       /* too dark -> caller falls back */
    long acc = 0;
    for (int v = 0; v < 1024; v++) { acc += hist[v]; if (acc >= n / 2) return v; }
    return -1.0;
}

/* Auto-find where the beam lands: brightness-weighted centroid over the frame, with the
 * per-pixel weight CAPPED so a blown retroreflector can't drag the center to itself. The
 * beam pointing is fixed (mechanical boresight offset), so a slow EMA of this in the
 * caller settles on the true beam center — no manual calibration. Returns 0 if the frame
 * has no lit content. */
int isp_lit_centroid(const uint8_t *y10, int bpl, int w, int h, double *ocx, double *ocy)
{
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8) {
            int v = y10_at(y10, bpl, x, y) - (int)EO_BLACK;
            if (v <= 8) continue;
            if (v > 300) v = 300;                  /* cap: speculars don't dominate */
            sx += (double)x * v; sy += (double)y * v; sw += v;
        }
    if (sw < 1) return 0;
    *ocx = sx / sw; *ocy = sy / sw;
    return 1;
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

/* 8x8 ordered (Bayer) dither, 0..15 after >>2 — STATIC pattern, temporally stable
 * (a per-frame random dither shimmers at 60 Hz and inflates the MJPEG bitrate). */
static const uint8_t g_bayer8[64] = {
     0,32, 8,40, 2,34,10,42,  48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38,  60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41,  51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37,  63,31,55,23,61,29,53,21,
};

/* Crop (cx,cy,cw,ch) of the Y10 frame, box-average downscale to ow*oh, then
 * black-level + adaptive-white tone map + gamma -> 8-bit. The crop is the digital
 * zoom; box-average anti-aliases the downscale. One pass, small output.
 * in_q5: 0 = raw packed Y10 (10-bit in bits [15:6]); 1 = denoised Q10.5 (bits [15:1]).
 * Internally everything is Q10.5, and the 8-bit quantization is LUT-interpolated +
 * ordered-dithered — post-denoise the temporal noise no longer self-dithers, and a
 * ~6x night stretch of clean data contours visibly if simply truncated. */
void isp_scale_tonemap(const uint8_t *y10, int bpl, int cx, int cy, int cw, int ch,
                       uint8_t *out8, int ow, int oh, int in_q5)
{
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
    int shr = in_q5 ? 1 : 6, shl = in_q5 ? 0 : 5;      /* load as Q10.5 either way */
    for (int oy = 0; oy < oh; oy++) {
        int sy0 = cy + oy * ch / oh, sy1 = cy + (oy + 1) * ch / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
        uint16_t *orow = sm + (size_t)oy * ow;
        for (int ox = 0; ox < ow; ox++) {
            int sx0 = xs[ox], sx1 = xs[ox + 1]; if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned acc = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = y10 + (size_t)sy * bpl;
                for (int sx = sx0; sx < sx1; sx++)
                    { acc += (unsigned)(((row[2*sx] | (row[2*sx+1] << 8)) >> shr) << shl); cnt++; }
            }
            orow[ox] = (uint16_t)(cnt ? acc / cnt : 0);
        }
    }
    /* p1/p99 percentile endpoints on the 10-bit values. p99 (not max/99.5%) ignores a
     * small blown streetlight; p1 sets a real black. Endpoints are EMA-smoothed across
     * frames so the mapping doesn't wobble frame-to-frame (the "breathing"), and the
     * span is floored so a flat/dim scene isn't blown up to full range (6x noise gain).*/
    int hist[1024] = {0}, nh = 0;
    for (int i = 0; i < npx; i += 4) { hist[sm[i] >> 5]++; nh++; }   /* subsample */
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
    /* stretch + gamma folded into a per-frame LUT in Q4 (4 fractional output bits);
     * per pixel: interpolate between LUT entries with the Q10.5 fraction, add the
     * ordered dither, quantize. Sub-LSB detail survives to 8-bit instead of banding. */
    uint16_t map16[1025];
    for (int v = 0; v <= 1024; v++) {
        double s = (v - lo) * scale;
        if (s < 0) s = 0;
        if (s > 255) s = 255;
        map16[v] = (uint16_t)lround(pow(s / 255.0, 1.0 / EO_GAMMA) * 255.0 * 16.0);
    }
    for (int oy = 0; oy < oh; oy++) {
        const uint16_t *srow = sm + (size_t)oy * ow;
        uint8_t *drow = out8 + (size_t)oy * ow;
        const uint8_t *brow = g_bayer8 + (oy & 7) * 8;
        for (int ox = 0; ox < ow; ox++) {
            int v5 = srow[ox], v = v5 >> 5, f = v5 & 31;
            int m = map16[v] + (((map16[v + 1] - map16[v]) * f) >> 5);
            int q = (m + (brow[ox & 7] >> 2)) >> 4;
            drow[ox] = (uint8_t)(q > 255 ? 255 : q);
        }
    }
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
