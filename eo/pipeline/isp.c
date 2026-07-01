/* Light mono ISP: Y10 (left-justified 16-bit, >>6) -> black-level + adaptive-white
 * tone map -> gamma -> 8-bit. Matches the bench tool. The detector can instead
 * consume the linear 10-bit directly; this path is for the monitor feed. */
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

/* 99.5th-percentile of a 4x4 subsample via a 1024-bin histogram (adaptive white). */
static double white_point(const uint8_t *y10, int bpl, int w, int h)
{
    int hist[1024] = {0}, n = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) { hist[y10_at(y10, bpl, x, y)]++; n++; }
    int target = (int)(n * 0.995), acc = 0;
    for (int v = 0; v < 1024; v++) { acc += hist[v]; if (acc >= target) return v; }
    return 1023.0;
}

void isp_tonemap(const uint8_t *y10, int bpl, int w, int h, uint8_t *out8)
{
    if (!g_lut_ready) lut_init();
    double white = white_point(y10, bpl, w, h);
    if (white <= EO_BLACK + 1) white = EO_BLACK + 1;
    double scale = 255.0 / (white - EO_BLACK);

    for (int y = 0; y < h; y++) {
        const uint8_t *row = y10 + (size_t)y * bpl;
        uint8_t *o = out8 + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int v10 = (row[2 * x] | (row[2 * x + 1] << 8)) >> 6;
            double s = (v10 - EO_BLACK) * scale;
            int i = (int)(s < 0 ? 0 : s > 255 ? 255 : s);
            o[x] = g_lut[i];
        }
    }
}

/* Digital zoom: upscale a centered 1/zoom crop back to w*h (nearest-neighbour).
 * The source column index is precomputed once per row-set (xmap) instead of a
 * per-pixel divide-by-width, which kept zoom at ~60 fps instead of dropping to ~49. */
void isp_zoom(const uint8_t *src, int w, int h, int zoom, uint8_t *dst)
{
    if (zoom <= 1) { for (size_t i = 0; i < (size_t)w * h; i++) dst[i] = src[i]; return; }
    int cw = w / zoom, ch = h / zoom;
    int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    int *xmap = malloc((size_t)w * sizeof(int));
    if (!xmap) { for (size_t i = 0; i < (size_t)w * h; i++) dst[i] = src[i]; return; }
    for (int x = 0; x < w; x++) xmap[x] = x0 + x * cw / w;   /* divide once, not per pixel */
    for (int y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)(y0 + y * ch / h) * w;
        uint8_t *drow = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) drow[x] = srow[xmap[x]];
    }
    free(xmap);
}

/* Focus-assist sharpness: mean squared gradient (Tenengrad) over a centered ROI.
 * Higher = sharper. Absolute scale is arbitrary; the UI shows it as a % of the
 * peak seen while focusing. Cheap (center ~40% box, 8-bit). */
double isp_sharpness(const uint8_t *img8, int w, int h)
{
    int x0 = w * 3 / 10, x1 = w * 7 / 10;
    int y0 = h * 3 / 10, y1 = h * 7 / 10;
    uint64_t sum = 0; int n = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *r = img8 + (size_t)y * w;
        for (int x = x0; x < x1; x++) {
            int gx = r[x + 1] - r[x - 1];
            int gy = r[x + w] - r[x - w];
            sum += (uint64_t)(gx * gx + gy * gy);
            n++;
        }
    }
    return n ? (double)sum / n : 0.0;
}
