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
    int hist[1024] = {0};
    for (int i = 0; i < npx; i++) hist[sm[i]]++;
    int target = (int)(npx * 0.995), a = 0, white = 1023;
    for (int v = 0; v < 1024; v++) { a += hist[v]; if (a >= target) { white = v; break; } }
    if (white <= EO_BLACK + 1) white = (int)EO_BLACK + 1;
    double scale = 255.0 / (white - EO_BLACK);
    for (int i = 0; i < npx; i++) {
        double s = (sm[i] - EO_BLACK) * scale;
        int q = (int)(s < 0 ? 0 : s > 255 ? 255 : s);
        out8[i] = g_lut[q];
    }
    free(sm);
}
