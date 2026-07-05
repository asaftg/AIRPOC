/* Light mono ISP for the wire feed. Two jobs:
 *   - metrics on the NATIVE frame: AE mean (isp_mean10) + focus sharpness (isp_sharpness)
 *   - isp_scale_tonemap: crop(zoom) + black-level/adaptive-white tone map + gamma in one
 *     pass. Output is native resolution (ow=cw, oh=ch): no downscale, full detail. It
 *     supports box-average downscale if ever asked (ow<cw), but the feed runs 1:1.
 *     The detector uses the raw Y10; tone/gamma here is for the human view only. */
#include "pipeline.h"
#include "eo_tonemap.h"        /* canonical tone map + median (shared with the recorder) */
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
/* Live-feed tone map: delegates to the canonical shared unit (eo_tonemap.c),
 * which the recorder also compiles so native replay is pixel-identical. This
 * wrapper only owns the hot-path statics (EMA state + malloc-free scratch). */
void isp_scale_tonemap(const uint8_t *y10, int bpl, int cx, int cy, int cw, int ch,
                       uint8_t *out8, int ow, int oh)
{
    int npx = ow * oh;
    static uint16_t *sm = NULL;  static int sm_cap = 0;
    static int      *xs = NULL;  static int xs_cap = 0;
    static EoToneState st;                               /* zero-init: seeds on first frame */
    if (npx > sm_cap) { free(sm); sm = malloc((size_t)npx * sizeof(uint16_t)); sm_cap = sm ? npx : 0; }
    if (ow + 1 > xs_cap) { free(xs); xs = malloc((size_t)(ow + 1) * sizeof(int)); xs_cap = xs ? ow + 1 : 0; }
    if (!sm || !xs) return;
    eo_tonemap(y10, bpl, cx, cy, cw, ch, out8, ow, oh, &st, sm, xs);
}

void isp_median3(uint8_t *img, int w, int h)
{
    static uint8_t *src = NULL; static int cap = 0;      /* hot path: no per-frame malloc */
    if (w * h > cap) { free(src); src = malloc((size_t)w * h); cap = src ? w * h : 0; }
    eo_median3(img, w, h, src);
}
