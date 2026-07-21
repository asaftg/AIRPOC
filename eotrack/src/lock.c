#include "lock.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NG   24            /* correlation grid side (NG*NG samples) */

struct Lock {
    int    has;
    double half_w, half_h; /* template half-extent in px at capture scale */
    float  t[NG * NG];      /* template samples */
    double tmean, tnorm;    /* precomputed template mean and L2 norm of (t-mean) */
};

Lock *lock_new(void) { return calloc(1, sizeof(Lock)); }
void  lock_free(Lock *l) { free(l); }
void  lock_reset(Lock *l) { if (l) l->has = 0; }
int   lock_has_template(const Lock *l) { return l && l->has; }

static inline int clampi(int v, int lo, int hi){ return v<lo?lo:v>hi?hi:v; }

/* Sample NG x NG points spanning [cx-hw, cx+hw] x [cy-hh, cy+hh] into g[], and
 * return mean; also fill *norm with sqrt(sum (g-mean)^2). Points outside the frame
 * clamp to the edge. */
static double sample_grid(const uint16_t *frame, int w, int h,
                          double cx, double cy, double hw, double hh,
                          float *g, double *norm)
{
    double sum = 0;
    for (int j = 0; j < NG; j++) {
        double fy = cy + (( (j + 0.5) / NG) * 2.0 - 1.0) * hh;
        int iy = clampi((int)(fy + 0.5), 0, h - 1);
        for (int i = 0; i < NG; i++) {
            double fx = cx + (((i + 0.5) / NG) * 2.0 - 1.0) * hw;
            int ix = clampi((int)(fx + 0.5), 0, w - 1);
            float v = (float)frame[(size_t)iy * w + ix];
            g[j * NG + i] = v;
            sum += v;
        }
    }
    double mean = sum / (NG * NG);
    double ss = 0;
    for (int k = 0; k < NG * NG; k++) { double d = g[k] - mean; ss += d * d; }
    if (norm) *norm = sqrt(ss);
    return mean;
}

void lock_set_template(Lock *l, const uint16_t *frame, int w, int h,
                       double cx, double cy, double bw, double bh)
{
    if (!l) return;
    double hw = 0.5 * bw, hh = 0.5 * bh;
    if (hw < 4) hw = 4;
    if (hh < 4) hh = 4;
    if (hw > TRK_LOCK_ROI_MAX / 2) hw = TRK_LOCK_ROI_MAX / 2;
    if (hh > TRK_LOCK_ROI_MAX / 2) hh = TRK_LOCK_ROI_MAX / 2;
    l->half_w = hw; l->half_h = hh;
    l->tmean = sample_grid(frame, w, h, cx, cy, hw, hh, l->t, &l->tnorm);
    if (l->tnorm < 1e-3) l->tnorm = 1e-3;
    l->has = 1;
}

/* NCC of the template against a candidate window centred at (cx,cy) scaled by s. */
static double ncc_at(const Lock *l, const uint16_t *frame, int w, int h,
                     double cx, double cy, double s)
{
    float g[NG * NG]; double gnorm;
    double gmean = sample_grid(frame, w, h, cx, cy, l->half_w * s, l->half_h * s, g, &gnorm);
    if (gnorm < 1e-3) return -1;
    double cross = 0;
    for (int k = 0; k < NG * NG; k++)
        cross += (l->t[k] - l->tmean) * (g[k] - gmean);
    return cross / (l->tnorm * gnorm);
}

int lock_track(Lock *l, const uint16_t *frame, int w, int h,
               double px, double py, double *ox, double *oy, double *score)
{
    if (!l || !l->has) return 0;
    static const double scales[3] = { 0.9, 1.0, 1.1 };
    double best = -2, bx = px, by = py;
    for (int si = 0; si < 3; si++) {
        for (int dy = -TRK_LOCK_SEARCH; dy <= TRK_LOCK_SEARCH; dy += 2) {
            for (int dx = -TRK_LOCK_SEARCH; dx <= TRK_LOCK_SEARCH; dx += 2) {
                double cx = px + dx, cy = py + dy;
                double n = ncc_at(l, frame, w, h, cx, cy, scales[si]);
                if (n > best) { best = n; bx = cx; by = cy; }
            }
        }
    }
    if (ox) *ox = bx;
    if (oy) *oy = by;
    if (score) *score = best;
    return 1;
}
