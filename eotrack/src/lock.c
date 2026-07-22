/* lock.c - sparse pyramidal Lucas-Kanade optical-flow lock (see lock.h).
 *
 * Pipeline per engaged target:
 *   lock_anchor(): store the frame as "previous" and detect Shi-Tomasi corners on a grid
 *     inside the target box.
 *   lock_track(): build small Gaussian pyramids of the previous and current frames over a
 *     WINDOW around the tracked points (never the whole frame - that would cost ~5 ms), run
 *     forward-additive LK per point coarse-to-fine (ego shift as the initial guess), keep
 *     only points that pass a forward-backward consistency check, take the robust MEDIAN of
 *     their translations as the target's motion, advance the centre, and roll cur -> prev.
 *
 * Windowing is the key to the budget: the target box is <= ~192 px, so a ~380 px window
 * covers the points + a shake's worth of motion + the pyramid apron, ~20x less pixel work
 * than a full 1440x1088 pyramid. No allocations on the hot path.
 */
#include "lock.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LV       TRK_LK_LEVELS
#define WIN      TRK_LK_WIN
#define MAXWIN   512                      /* max window side (px) at level 0 */
#define WMARGIN  64                       /* apron around the point bbox (motion + pyramid) */

/* A Gaussian pyramid of a WINDOW of one frame, float. L0 = window at full res; (ox,oy) is
 * the frame-pixel coordinate of window-local (0,0). Both pyramids in a lock_track call use
 * the SAME window, so points map by a plain origin subtraction. */
typedef struct {
    float *L[LV];
    int    w[LV], h[LV];
    int    ox, oy;
} Pyr;

struct Lock {
    int       has;
    double    cx, cy;                     /* tracked centre, frame px */
    double    hw, hh;                     /* box half-extent for corner (re)detection, px */
    int       npts, init_pts;
    float     px[TRK_LK_MAX_PTS];         /* point positions in the PREVIOUS frame, frame px */
    float     py[TRK_LK_MAX_PTS];
    uint16_t *prev;                       /* previous full frame (Y10) */
    int       pw, ph, prev_valid;
    Pyr       pa, pb;                     /* scratch window pyramids (prev, cur) */
};

static inline int    clampi(int v, int lo, int hi){ return v<lo?lo:v>hi?hi:v; }
static inline double clampd(double v,double lo,double hi){ return v<lo?lo:v>hi?hi:v; }

/* ---- sampling ------------------------------------------------------------ */

/* Bilinear sample of a float image with edge clamp. */
static inline float sampf(const float *img, int w, int h, double x, double y)
{
    if (x < 0) x = 0; else if (x > w - 1) x = w - 1;
    if (y < 0) y = 0; else if (y > h - 1) y = h - 1;
    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    double ax = x - x0, ay = y - y0;
    const float *r0 = img + (size_t)y0 * w, *r1 = img + (size_t)y1 * w;
    double a = r0[x0] + (r0[x1] - r0[x0]) * ax;
    double b = r1[x0] + (r1[x1] - r1[x0]) * ax;
    return (float)(a + (b - a) * ay);
}

/* Bilinear sample of the raw Y10 (uint16) frame with edge clamp (for corner detection). */
static inline double sampu(const uint16_t *img, int w, int h, double x, double y)
{
    if (x < 0) x = 0; else if (x > w - 1) x = w - 1;
    if (y < 0) y = 0; else if (y > h - 1) y = h - 1;
    int x0 = (int)x, y0 = (int)y;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    double ax = x - x0, ay = y - y0;
    const uint16_t *r0 = img + (size_t)y0 * w, *r1 = img + (size_t)y1 * w;
    double a = r0[x0] + ((double)r0[x1] - r0[x0]) * ax;
    double b = r1[x0] + ((double)r1[x1] - r1[x0]) * ax;
    return a + (b - a) * ay;
}

/* ---- window pyramid ------------------------------------------------------ */

static void pyr_alloc(Pyr *p)
{
    int s = MAXWIN;
    for (int l = 0; l < LV; l++) {
        p->L[l] = malloc((size_t)(s + 2) * (s + 2) * sizeof(float));
        s = (s + 1) / 2;
    }
}
static void pyr_free(Pyr *p){ for (int l = 0; l < LV; l++){ free(p->L[l]); p->L[l]=NULL; } }

/* Downsample src -> dst by 2 with a 3x3 Gaussian [1 2 1]^2/16. */
static void downsample(const float *src, int sw, int sh, float *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy = 2 * y;
        int y0 = clampi(sy-1,0,sh-1), y1 = clampi(sy,0,sh-1), y2 = clampi(sy+1,0,sh-1);
        const float *r0 = src+(size_t)y0*sw, *r1 = src+(size_t)y1*sw, *r2 = src+(size_t)y2*sw;
        for (int x = 0; x < dw; x++) {
            int sx = 2 * x;
            int x0 = clampi(sx-1,0,sw-1), x1 = clampi(sx,0,sw-1), x2 = clampi(sx+1,0,sw-1);
            dst[(size_t)y*dw+x] = (r0[x0]+2*r0[x1]+r0[x2]
                                 + 2*(r1[x0]+2*r1[x1]+r1[x2])
                                 + (r2[x0]+2*r2[x1]+r2[x2])) * (1.0f/16.0f);
        }
    }
}

/* Build a window pyramid of `frame` over the rect (wx,wy,ww,wh) (clamped to <= MAXWIN). */
static void pyr_build_win(Pyr *p, const uint16_t *frame, int fw, int fh,
                          int wx, int wy, int ww, int wh)
{
    if (ww > MAXWIN) ww = MAXWIN;
    if (wh > MAXWIN) wh = MAXWIN;
    p->ox = wx; p->oy = wy; p->w[0] = ww; p->h[0] = wh;
    for (int y = 0; y < wh; y++) {
        int fy = clampi(wy + y, 0, fh - 1);
        const uint16_t *sr = frame + (size_t)fy * fw;
        float *dr = p->L[0] + (size_t)y * ww;
        for (int x = 0; x < ww; x++) dr[x] = (float)sr[clampi(wx + x, 0, fw - 1)];
    }
    for (int l = 1; l < LV; l++) {
        p->w[l] = (p->w[l-1] + 1) / 2;
        p->h[l] = (p->h[l-1] + 1) / 2;
        downsample(p->L[l-1], p->w[l-1], p->h[l-1], p->L[l], p->w[l], p->h[l]);
    }
}

/* ---- single-point Lucas-Kanade ------------------------------------------- */

/* Refine flow (gx,gy) [level-l px] tracking point (x,y) [level-l local px] from image I to
 * J. Forward-additive LK over a WSIDE window. 0 if the window is degenerate/out of bounds. */
static int lk_level(const float *I, const float *J, int w, int h,
                    double x, double y, double *gx, double *gy)
{
    double m = WIN + 2;
    if (x < m || y < m || x > w - 1 - m || y > h - 1 - m) return 0;

    static float Ix[(2*WIN+1)*(2*WIN+1)], Iy[(2*WIN+1)*(2*WIN+1)], I0[(2*WIN+1)*(2*WIN+1)];
    double G00 = 0, G01 = 0, G11 = 0;
    int k = 0;
    for (int wy = -WIN; wy <= WIN; wy++)
        for (int wx = -WIN; wx <= WIN; wx++, k++) {
            double sx = x + wx, sy = y + wy;
            float ix = 0.5f*(sampf(I,w,h,sx+1,sy) - sampf(I,w,h,sx-1,sy));
            float iy = 0.5f*(sampf(I,w,h,sx,sy+1) - sampf(I,w,h,sx,sy-1));
            Ix[k]=ix; Iy[k]=iy; I0[k]=sampf(I,w,h,sx,sy);
            G00 += (double)ix*ix; G01 += (double)ix*iy; G11 += (double)iy*iy;
        }
    double det = G00*G11 - G01*G01;
    if (det < 1e-6) return 0;
    double i00 = G11/det, i01 = -G01/det, i11 = G00/det;

    for (int it = 0; it < TRK_LK_ITERS; it++) {
        double b0 = 0, b1 = 0;
        k = 0;
        for (int wy = -WIN; wy <= WIN; wy++)
            for (int wx = -WIN; wx <= WIN; wx++, k++) {
                double It = (double)sampf(J,w,h, x+*gx+wx, y+*gy+wy) - I0[k];
                b0 += Ix[k]*It; b1 += Iy[k]*It;
            }
        double dvx = -(i00*b0 + i01*b1), dvy = -(i01*b0 + i11*b1);
        *gx += dvx; *gy += dvy;
        if (dvx*dvx + dvy*dvy < 1e-4) break;
    }
    return 1;
}

/* Track absolute point (x0,y0) from window pyramid A to B (same window), guess (gx,gy) in
 * frame px. Writes matched absolute point (*ox,*oy). Returns 1 on success. */
static int lk_track_pt(const Pyr *A, const Pyr *B, double x0, double y0,
                       double gx, double gy, double *ox, double *oy)
{
    double lx0 = x0 - A->ox, ly0 = y0 - A->oy;      /* window-local level-0 coords */
    double dx = gx, dy = gy;
    for (int l = LV - 1; l >= 0; l--) {
        double s = 1.0 / (1 << l);
        double xl = lx0*s, yl = ly0*s, dlx = dx*s, dly = dy*s;
        if (!lk_level(A->L[l], B->L[l], A->w[l], A->h[l], xl, yl, &dlx, &dly)) return 0;
        dx = dlx*(1<<l); dy = dly*(1<<l);
    }
    *ox = x0 + dx; *oy = y0 + dy;
    return 1;
}

/* ---- corner detection (on the raw prev frame) ---------------------------- */

static double min_eig(const uint16_t *I, int w, int h, double x, double y)
{
    double G00 = 0, G01 = 0, G11 = 0;
    for (int wy = -WIN; wy <= WIN; wy++)
        for (int wx = -WIN; wx <= WIN; wx++) {
            double sx = x+wx, sy = y+wy;
            double ix = 0.5*(sampu(I,w,h,sx+1,sy) - sampu(I,w,h,sx-1,sy));
            double iy = 0.5*(sampu(I,w,h,sx,sy+1) - sampu(I,w,h,sx,sy-1));
            G00 += ix*ix; G01 += ix*iy; G11 += iy*iy;
        }
    double t = G00 + G11, d = G00*G11 - G01*G01;
    double disc = sqrt(fmax(0.0, t*t*0.25 - d));
    return t*0.5 - disc;
}

/* Detect up to TRK_LK_MAX_PTS strong corners on a grid inside the box, on the raw frame. */
static void detect_corners(Lock *l, const uint16_t *F, int w, int h)
{
    double m = WIN + 2 + (1 << (LV - 1));
    double x0 = clampd(l->cx - l->hw, m, w-1-m), x1 = clampd(l->cx + l->hw, m, w-1-m);
    double y0 = clampd(l->cy - l->hh, m, h-1-m), y1 = clampd(l->cy + l->hh, m, h-1-m);
    int G = TRK_LK_GRID;

    static double eig[TRK_LK_GRID*TRK_LK_GRID], gx[TRK_LK_GRID*TRK_LK_GRID], gy[TRK_LK_GRID*TRK_LK_GRID];
    double emax = 0;
    int gc = 0;
    for (int j = 0; j < G; j++) {
        double yy = (y1 > y0) ? y0 + (y1-y0)*(j+0.5)/G : l->cy;
        for (int i = 0; i < G; i++, gc++) {
            double xx = (x1 > x0) ? x0 + (x1-x0)*(i+0.5)/G : l->cx;
            double e = min_eig(F, w, h, xx, yy);
            eig[gc]=e; gx[gc]=xx; gy[gc]=yy;
            if (e > emax) emax = e;
        }
    }
    double thr = fmax(TRK_LK_EIG_FLOOR, 0.05*emax);
    int n = 0;
    for (int k = 0; k < gc && n < TRK_LK_MAX_PTS; k++)
        if (eig[k] >= thr) { l->px[n]=(float)gx[k]; l->py[n]=(float)gy[k]; n++; }
    if (n == 0 && gc > 0) {
        int best = 0; for (int k = 1; k < gc; k++) if (eig[k] > eig[best]) best = k;
        l->px[0]=(float)gx[best]; l->py[0]=(float)gy[best]; n = 1;
    }
    l->npts = n; l->init_pts = n;
}

/* ---- public API ---------------------------------------------------------- */

Lock *lock_new(void)
{
    Lock *l = calloc(1, sizeof(Lock));
    if (!l) return NULL;
    l->prev = malloc((size_t)EO_IMG_W * EO_IMG_H * sizeof(uint16_t));
    pyr_alloc(&l->pa); pyr_alloc(&l->pb);
    return l;
}
void lock_free(Lock *l)
{
    if (!l) return;
    free(l->prev); pyr_free(&l->pa); pyr_free(&l->pb);
    free(l);
}
void lock_reset(Lock *l){ if (l){ l->has = 0; l->prev_valid = 0; l->npts = 0; } }
int  lock_has_template(const Lock *l){ return l && l->has && l->prev_valid && l->npts > 0; }

void lock_anchor(Lock *l, const uint16_t *frame, int w, int h,
                 double cx, double cy, double bw, double bh)
{
    if (!l) return;
    double hw = 0.5 * bw, hh = 0.5 * bh;
    if (hw < 4) hw = 4;
    if (hh < 4) hh = 4;
    if (hw > TRK_LOCK_ROI_MAX / 2) hw = TRK_LOCK_ROI_MAX / 2;
    if (hh > TRK_LOCK_ROI_MAX / 2) hh = TRK_LOCK_ROI_MAX / 2;
    l->cx = cx; l->cy = cy; l->hw = hw; l->hh = hh;

    memcpy(l->prev, frame, (size_t)w * h * sizeof(uint16_t));
    l->pw = w; l->ph = h; l->prev_valid = 1;
    detect_corners(l, l->prev, w, h);
    l->has = 1;
}

static int cmp_d(const void *a, const void *b)
{
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}
static double median(double *v, int n)
{
    qsort(v, n, sizeof(double), cmp_d);
    return (n & 1) ? v[n/2] : 0.5 * (v[n/2 - 1] + v[n/2]);
}

int lock_track(Lock *l, const uint16_t *frame, int w, int h,
               double ego_dx, double ego_dy,
               double *ox, double *oy, double *score)
{
    if (!lock_has_template(l)) return 0;

    /* window = point bbox, extended along the ego guess, plus an apron */
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (int i = 0; i < l->npts; i++) {
        if (l->px[i] < minx) minx = l->px[i];
        if (l->px[i] > maxx) maxx = l->px[i];
        if (l->py[i] < miny) miny = l->py[i];
        if (l->py[i] > maxy) maxy = l->py[i];
    }
    double ex0 = ego_dx < 0 ? ego_dx : 0, ex1 = ego_dx > 0 ? ego_dx : 0;
    double ey0 = ego_dy < 0 ? ego_dy : 0, ey1 = ego_dy > 0 ? ego_dy : 0;
    int wx = (int)(minx + ex0) - WMARGIN, wy = (int)(miny + ey0) - WMARGIN;
    int ww = (int)(maxx + ex1) - wx + WMARGIN + 1, wh = (int)(maxy + ey1) - wy + WMARGIN + 1;

    pyr_build_win(&l->pa, l->prev, l->pw, l->ph, wx, wy, ww, wh);   /* previous */
    pyr_build_win(&l->pb, frame,   w,     h,     wx, wy, ww, wh);   /* current  */

    double dxs[TRK_LK_MAX_PTS], dys[TRK_LK_MAX_PTS];
    float  nx[TRK_LK_MAX_PTS], ny[TRK_LK_MAX_PTS];
    int surv = 0;
    for (int i = 0; i < l->npts; i++) {
        double fx, fy, bx, by;
        if (!lk_track_pt(&l->pa, &l->pb, l->px[i], l->py[i], ego_dx, ego_dy, &fx, &fy)) continue;
        if (!lk_track_pt(&l->pb, &l->pa, fx, fy, -ego_dx, -ego_dy, &bx, &by)) continue;   /* FB */
        if (hypot(bx - l->px[i], by - l->py[i]) > TRK_LK_FB_MAX) continue;
        dxs[surv] = fx - l->px[i]; dys[surv] = fy - l->py[i];
        nx[surv] = (float)fx; ny[surv] = (float)fy;
        surv++;
    }

    if (surv == 0) {
        /* nothing tracked: hold the centre + previous frame; caller HOLDs on the 0 score. */
        if (ox) *ox = l->cx;
        if (oy) *oy = l->cy;
        if (score) *score = 0.0;
        return 1;
    }

    double mdx = median(dxs, surv), mdy = median(dys, surv);

    /* score = inlier CONSENSUS (how tightly the survivors agree on the motion), scaled down
     * when very few points survived. A clean track has many survivors that all agree -> ~1;
     * a shake onto a distractor or a lost target scatters the flows -> low -> caller HOLDs.
     * This is why position can be exact with only ~40% surviving the FB check: the survivors
     * agree even when edge points (aperture problem) get pruned. */
    int inl = 0;
    for (int i = 0; i < surv; i++)
        if (hypot(dxs[i] - mdx, dys[i] - mdy) <= TRK_LK_INLIER) inl++;
    double consensus = (double)inl / surv;
    double sufficiency = clampd((double)surv / TRK_LK_MIN_PTS, 0.0, 1.0);
    double sc = consensus * sufficiency;

    l->cx += mdx; l->cy += mdy;

    for (int i = 0; i < surv; i++) { l->px[i] = nx[i]; l->py[i] = ny[i]; }
    l->npts = surv;

    /* roll current -> previous */
    memcpy(l->prev, frame, (size_t)w * h * sizeof(uint16_t));
    l->pw = w; l->ph = h;

    /* replenish points if the survivor set thinned out (keeps flow alive between anchors) */
    if (l->npts < TRK_LK_MIN_PTS)
        detect_corners(l, l->prev, w, h);

    if (ox) *ox = l->cx;
    if (oy) *oy = l->cy;
    if (score) *score = sc;
    return 1;
}
