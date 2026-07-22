#include "ego.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Camera-motion (ego) estimate = the global translation between consecutive EO frames,
 * found by a coarse-to-fine 2D block match on a downsampled, mean-subtracted image.
 *
 * The old row/column-projection method saturated at ~48 px, but a hand-moved sensor
 * shifts the scene up to ~240 px per detector tick (measured), which is exactly when the
 * tracker needs the estimate most. A 2D block match at 1/8 resolution, searched coarsely
 * out to +/-288 px then refined to 1 px, handles that for a few million ops per call.
 * Mean-subtracting each frame makes it robust to the exposure/illuminator changes that a
 * plain SSD would mistake for motion. */
#define EGO_DEC     8
#define EGO_W       (EO_IMG_W / EGO_DEC)     /* 180 */
#define EGO_H       (EO_IMG_H / EGO_DEC)     /* 136 */
#define EGO_COARSE  18      /* +/- bins at 2x-subsampled (factor 16) => +/-288 px */
#define EGO_FINE    4       /* +/- bins refined at factor 8 => +/-32 px */

struct Ego {
    int   have_prev;
    float prev[EGO_W * EGO_H];   /* previous frame, downsampled + mean-subtracted */
    float cur[EGO_W * EGO_H];
};

Ego *ego_new(void) { return calloc(1, sizeof(Ego)); }
void ego_free(Ego *e) { free(e); }

/* Mean SSD of cur vs prev shifted by (sx,sy), over their overlap. `sub` = spatial
 * subsample step (2 = coarse at factor 16, 1 = fine at factor 8). Returns 1e30 if the
 * overlap is too small to trust. */
static double ssd_at(const float *cur, const float *prev, int sx, int sy, int sub)
{
    double acc = 0; long m = 0;
    for (int y = 0; y < EGO_H; y += sub) {
        int py = y + sy;
        if (py < 0 || py >= EGO_H) continue;
        const float *cr = cur + (size_t)y * EGO_W;
        const float *pr = prev + (size_t)py * EGO_W;
        for (int x = 0; x < EGO_W; x += sub) {
            int px = x + sx;
            if (px < 0 || px >= EGO_W) continue;
            double d = cr[x] - pr[px];
            acc += d * d; m++;
        }
    }
    if (m < (EGO_W * EGO_H) / (sub * sub) / 4) return 1e30;   /* too little overlap */
    return acc / m;
}

void ego_update(Ego *e, const uint16_t *frame, int w, int h, double *dx, double *dy)
{
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    if (w != EO_IMG_W || h != EO_IMG_H) return;

    /* downsample (box average) + mean-subtract */
    double sum = 0;
    for (int ry = 0; ry < EGO_H; ry++) {
        for (int cx = 0; cx < EGO_W; cx++) {
            const uint16_t *p = frame + (size_t)(ry * EGO_DEC) * w + cx * EGO_DEC;
            float v = (float)*p;
            e->cur[ry * EGO_W + cx] = v;
            sum += v;
        }
    }
    float mean = (float)(sum / (EGO_W * EGO_H));
    for (int i = 0; i < EGO_W * EGO_H; i++) e->cur[i] -= mean;

    if (e->have_prev) {
        /* coarse search at factor 16 (sub=2), +/-EGO_COARSE bins step 2 */
        double best = 1e30, mean_ssd = 0; int cbx = 0, cby = 0, cnt = 0;
        for (int sy = -EGO_COARSE; sy <= EGO_COARSE; sy += 2)
            for (int sx = -EGO_COARSE; sx <= EGO_COARSE; sx += 2) {
                double s = ssd_at(e->cur, e->prev, sx, sy, 2);
                if (s >= 1e30) continue;
                mean_ssd += s; cnt++;
                if (s < best) { best = s; cbx = sx; cby = sy; }
            }
        mean_ssd = cnt ? mean_ssd / cnt : 0;
        /* refine at factor 8 (sub=1), +/-EGO_FINE bins step 1 around the coarse best */
        double fbest = 1e30; int fbx = cbx, fby = cby;
        for (int sy = cby - EGO_FINE; sy <= cby + EGO_FINE; sy++)
            for (int sx = cbx - EGO_FINE; sx <= cbx + EGO_FINE; sx++) {
                double s = ssd_at(e->cur, e->prev, sx, sy, 1);
                if (s < fbest) { fbest = s; fbx = sx; fby = sy; }
            }
        /* accept only a clear trough (real motion, not a flat/ambiguous match).
         * best_shift returns s where cur aligns to prev+s, so the scene MOVED by -s. */
        if (cnt > 0 && best < 0.9 * mean_ssd) {
            if (dx) *dx = -(double)fbx * EGO_DEC;
            if (dy) *dy = -(double)fby * EGO_DEC;
        }
    }
    memcpy(e->prev, e->cur, sizeof e->cur);
    e->have_prev = 1;
}
