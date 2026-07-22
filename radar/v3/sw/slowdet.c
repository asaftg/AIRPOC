/* slowdet.c — short-hop temporal chaining slow detector. See slowdet.h.
 *
 * Port of the offline-validated prototype (chain_dop.py / the standalone
 * slowdet C port). Streaming, allocation-free: a fixed ring of recent frames;
 * each frame's points kept range-sorted so a doppler-predicted landing is found
 * by binary search; thread identity flows forward by root inheritance down the
 * back-pointer. A point is declared once its chain reaches DECL_L hops over
 * SPAN_MIN seconds AND has physically travelled at >= MIN_SPEED (position, not
 * Doppler). Doppler only ever leads a hop; position decides the speed.
 */
#include "slowdet.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- tuning (identical to the validated prototype) ----------------------- */
#define SNR_MIN   15.0f
#define HOP_NEAR  0.5f
#define HOP_FAR   1.5f
#define HOP_R0    180.0f
#define HOP_R1    320.0f
#define MAX_HOP   1.5
#define DOP_LIVE  0.5f
#define DDOP_HOP  0.7f
#define DR_TOL    2.6f
#define DAZ_TOL   3.0f
#define NEED      6
#define DECL_L    (2 * NEED - 1)   /* = 11; Python's "thread holds NEED blue dots" */
#define SPAN_MIN  2.0
#define MIN_SPEED 0.5f
#define AZ_LIM    25.0f
#define R_MIN     15.0f
#define BOX_RWIN  6.0f
#define BOX_AWIN  3.5f

#define NPR       210
#define NPA       52
#define POCC_MAX  0.30f

#define MAXPTS    768
#define RINGF     50

typedef struct {
    double t;
    int    n;
    float  r[MAXPTS], az[MAXPTS], el[MAXPTS], dop[MAXPTS];
    float  x[MAXPTS], y[MAXPTS], z[MAXPTS];
    float  r0[MAXPTS];
    double t0[MAXPTS];
    int    L[MAXPTS];
    int    root[MAXPTS];
    unsigned char stat[MAXPTS];
} Frame;

struct SlowDet {
    Frame ring[RINGF];
    int   head, count;
    float pocc[NPR * NPA];
    int   next_id;
    float bestdd[MAXPTS];
    int   bestj[MAXPTS], bestf[MAXPTS];
};

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float hop_reach(float r) {
    return HOP_NEAR + (HOP_FAR - HOP_NEAR) *
           clampf((r - HOP_R0) / (HOP_R1 - HOP_R0), 0.0f, 1.0f);
}

SlowDet *slowdet_new(void) { return (SlowDet *)calloc(1, sizeof(SlowDet)); }
void     slowdet_free(SlowDet *sd) { free(sd); }

static int lower_bound_r(const float *r, int n, float key) {
    int lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) >> 1; if (r[m] < key) lo = m + 1; else hi = m; }
    return lo;
}

int slowdet_step(SlowDet *sd, const RadarPoint *pts, int n, double now_s,
                 RadarTarget *out, int max_out) {
    Frame nf;
    nf.t = now_s; nf.n = 0;

    /* cull + insertion-sort by range */
    for (int k = 0; k < n; k++) {
        float r = pts[k].range, az = pts[k].az;
        if (r <= R_MIN || fabsf(az) >= AZ_LIM || !(pts[k].snr >= SNR_MIN)) continue;
        if (nf.n >= MAXPTS) break;
        int i = nf.n++, p = i;
        while (p > 0 && nf.r[p - 1] > r) {
            nf.r[p]=nf.r[p-1]; nf.az[p]=nf.az[p-1]; nf.el[p]=nf.el[p-1]; nf.dop[p]=nf.dop[p-1];
            nf.x[p]=nf.x[p-1]; nf.y[p]=nf.y[p-1]; nf.z[p]=nf.z[p-1];
            p--;
        }
        nf.r[p]=r; nf.az[p]=az; nf.el[p]=pts[k].el; nf.dop[p]=pts[k].doppler;
        nf.x[p]=pts[k].x; nf.y[p]=pts[k].y; nf.z[p]=pts[k].z;
    }
    int m = nf.n;

    /* persistence: mark static, then decay the grid toward this frame's hits */
    for (int i = 0; i < m; i++) {
        int pr = (int)(nf.r[i] / 2.6f);   if (pr < 0) pr = 0; if (pr >= NPR) pr = NPR-1;
        int pa = (int)(nf.az[i] + 26.0f); if (pa < 0) pa = 0; if (pa >= NPA) pa = NPA-1;
        nf.stat[i] = sd->pocc[pr * NPA + pa] > POCC_MAX ? 1 : 0;
    }
    for (int c = 0; c < NPR * NPA; c++) sd->pocc[c] -= sd->pocc[c] / 64.0f;
    for (int i = 0; i < m; i++) {
        int pr = (int)(nf.r[i] / 2.6f);   if (pr < 0) pr = 0; if (pr >= NPR) pr = NPR-1;
        int pa = (int)(nf.az[i] + 26.0f); if (pa < 0) pa = 0; if (pa >= NPA) pa = NPA-1;
        sd->pocc[pr * NPA + pa] += 1.0f / 64.0f;
    }

    for (int i = 0; i < m; i++) {
        nf.L[i] = 1; nf.t0[i] = now_s; nf.r0[i] = nf.r[i]; nf.root[i] = sd->next_id++;
    }

    /* hop matching against each past frame, newest first */
    for (int fi = 0; fi < sd->count; fi++) {
        int slot = (sd->head + sd->count - 1 - fi) % RINGF;
        Frame *pf = &sd->ring[slot];
        double dt = now_s - pf->t;
        if (dt <= 0.02 || dt > MAX_HOP) { if (dt > MAX_HOP) break; else continue; }

        for (int i = 0; i < m; i++) sd->bestdd[i] = 1e30f;
        for (int j = 0; j < pf->n; j++) {
            if (fabsf(pf->dop[j]) < DOP_LIVE || pf->stat[j]) continue;
            float pred = pf->r[j] + pf->dop[j] * (float)dt;
            int lo = lower_bound_r(nf.r, m, pred - DR_TOL);
            for (int i = lo; i < m; i++) {
                if (nf.r[i] > pred + DR_TOL) break;
                if (dt > hop_reach(nf.r[i])) continue;
                if (fabsf(pf->az[j] - nf.az[i]) > DAZ_TOL) continue;
                float dd = fabsf(pf->dop[j] - nf.dop[i]);
                if (dd > DDOP_HOP) continue;
                if (dd < sd->bestdd[i]) { sd->bestdd[i]=dd; sd->bestj[i]=j; sd->bestf[i]=slot; }
            }
        }
        for (int i = 0; i < m; i++) {
            if (sd->bestdd[i] > 1e29f) continue;
            Frame *bp = &sd->ring[sd->bestf[i]];
            int j = sd->bestj[i], newL = bp->L[j] + 1;
            if (newL > nf.L[i]) {
                nf.L[i]=newL; nf.t0[i]=bp->t0[j]; nf.r0[i]=bp->r0[j]; nf.root[i]=bp->root[j];
            }
        }
    }

    /* declare + emit one target per thread (first/lowest-range dot per root) */
    int nout = 0;
    for (int i = 0; i < m; i++) {
        if (nf.stat[i] || nf.L[i] < DECL_L) continue;
        double span = now_s - nf.t0[i];
        if (span < SPAN_MIN) continue;
        float dr = fabsf(nf.r[i] - nf.r0[i]);
        float speed = dr / (float)span;
        if (speed < MIN_SPEED) continue;

        int dup = 0;
        for (int o = 0; o < nout; o++)
            if (out[o].tid == SLOWDET_TID_BASE + (nf.root[i] % 100000)) { dup = 1; break; }
        if (dup) continue;
        if (nout >= max_out) break;

        /* centroid + half-extent box from the echo cluster on this target */
        float cr = nf.r[i], ca = nf.az[i];
        double sx=0, sy=0, sz=0; float xlo=1e9f,xhi=-1e9f,ylo=1e9f,yhi=-1e9f,zlo=1e9f,zhi=-1e9f;
        int np = 0;
        int b = lower_bound_r(nf.r, m, cr - BOX_RWIN);
        for (int q = b; q < m; q++) {
            if (nf.r[q] > cr + BOX_RWIN) break;
            if (fabsf(nf.az[q] - ca) > BOX_AWIN) continue;
            sx += nf.x[q]; sy += nf.y[q]; sz += nf.z[q];
            if (nf.x[q] < xlo) xlo = nf.x[q];
            if (nf.x[q] > xhi) xhi = nf.x[q];
            if (nf.y[q] < ylo) ylo = nf.y[q];
            if (nf.y[q] > yhi) yhi = nf.y[q];
            if (nf.z[q] < zlo) zlo = nf.z[q];
            if (nf.z[q] > zhi) zhi = nf.z[q];
            np++;
        }
        RadarTarget *t = &out[nout++];
        if (np > 0) { t->x=(float)(sx/np); t->y=(float)(sy/np); t->z=(float)(sz/np); }
        else        { t->x=nf.x[i]; t->y=nf.y[i]; t->z=nf.z[i]; }
        t->sx = np>0 ? fmaxf((xhi-xlo)*0.5f, 0.5f) : 1.0f;
        t->sy = np>0 ? fmaxf((yhi-ylo)*0.5f, 0.5f) : 1.3f;
        t->sz = np>0 ? fmaxf((zhi-zlo)*0.5f, 0.5f) : 1.0f;
        /* position-derived radial velocity: signed range-rate along the LOS */
        float rr = (nf.r[i] - nf.r0[i]) / (float)span;     /* +receding */
        float inv = cr > 1e-3f ? rr / cr : 0.0f;
        t->vx = inv * t->x; t->vy = inv * t->y; t->vz = inv * t->z;
        t->conf = clampf((float)nf.L[i] / 20.0f, 0.3f, 1.0f);
        t->num_points = np;
        t->suspect = 0;
        t->mv_class = 1;                                   /* position-verified mover */
        t->tid = SLOWDET_TID_BASE + (nf.root[i] % 100000);
    }

    int wslot = (sd->head + sd->count) % RINGF;
    if (sd->count < RINGF) sd->count++;
    else sd->head = (sd->head + 1) % RINGF;
    sd->ring[wslot] = nf;
    return nout;
}
