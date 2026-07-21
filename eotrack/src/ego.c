#include "ego.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Decimation and projection sizes. We sample every DEC-th pixel and build a row
 * profile (length ~H/DEC) and a column profile (length ~W/DEC), then find the
 * 1-D shift (within +/-EGO_MAX decimated bins) that best aligns each profile to
 * the previous frame's. Cheap: a few thousand ops per frame. */
#define EGO_DEC   8
#define EGO_MAXW  ((EO_IMG_W / EGO_DEC) + 2)
#define EGO_MAXH  ((EO_IMG_H / EGO_DEC) + 2)
#define EGO_SEARCH 6      /* +/- decimated bins searched (=> +/-48 px at DEC=8) */

struct Ego {
    int    have_prev;
    int    nrow, ncol;
    double prow[EGO_MAXH], pcol[EGO_MAXW];   /* previous-frame profiles */
};

Ego *ego_new(void) { return calloc(1, sizeof(Ego)); }
void ego_free(Ego *e) { free(e); }

/* Best integer shift s in [-S,S] minimising SSD of a[] vs prev[] over the overlap;
 * returns the shift and sets *ok when the min is meaningfully below the mean SSD. */
static int best_shift(const double *cur, const double *prev, int n, int S, int *ok)
{
    double best = 1e300, mean = 0; int bs = 0, cnt = 0;
    for (int s = -S; s <= S; s++) {
        double ssd = 0; int m = 0;
        for (int i = 0; i < n; i++) {
            int j = i + s;
            if (j < 0 || j >= n) continue;
            double d = cur[i] - prev[j];
            ssd += d * d; m++;
        }
        if (m < n / 2) continue;
        ssd /= m;
        mean += ssd; cnt++;
        if (ssd < best) { best = ssd; bs = s; }
    }
    mean = cnt ? mean / cnt : 0;
    *ok = (cnt > 0 && best < 0.85 * mean);   /* a clear trough = real motion */
    return bs;
}

void ego_update(Ego *e, const uint16_t *frame, int w, int h, double *dx, double *dy)
{
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    int ncol = w / EGO_DEC, nrow = h / EGO_DEC;
    if (ncol > EGO_MAXW) ncol = EGO_MAXW;
    if (nrow > EGO_MAXH) nrow = EGO_MAXH;
    double crow[EGO_MAXH], ccol[EGO_MAXW];
    memset(crow, 0, sizeof(double) * nrow);
    memset(ccol, 0, sizeof(double) * ncol);
    for (int y = 0, ry = 0; ry < nrow; y += EGO_DEC, ry++) {
        const uint16_t *row = frame + (size_t)y * w;
        for (int x = 0, cx = 0; cx < ncol; x += EGO_DEC, cx++) {
            double v = row[x];
            crow[ry] += v;
            ccol[cx] += v;
        }
    }
    if (e->have_prev && e->nrow == nrow && e->ncol == ncol) {
        int okx, oky;
        int sx = best_shift(ccol, e->pcol, ncol, EGO_SEARCH, &okx);
        int sy = best_shift(crow, e->prow, nrow, EGO_SEARCH, &oky);
        /* best_shift returns s where cur[i] aligns to prev[i+s]; the image content
         * (a static point) therefore MOVED by -s bins. Report that image motion so
         * the core can subtract it from each track's stored path. */
        if (okx && dx) *dx = -(double)sx * EGO_DEC;
        if (oky && dy) *dy = -(double)sy * EGO_DEC;
    }
    memcpy(e->prow, crow, sizeof(double) * nrow);
    memcpy(e->pcol, ccol, sizeof(double) * ncol);
    e->nrow = nrow; e->ncol = ncol; e->have_prev = 1;
}
