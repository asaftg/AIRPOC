/* fuse.c — F+S merge and elevation conditioning. See fuse.h. */
#include "fuse.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (M_PI / 180.0)

/* merge: one box per object */
#define MATCH_R   15.0f    /* m   same object if within this range */
#define MATCH_A    5.0f    /* deg ...and this bearing */

/* elevation conditioning */
#define EL_BASE    0.6     /* s   trailing window at 200 m */
#define EL_WMIN    0.3
#define EL_WMAX    1.2
#define EL_MAXRATE 20.0    /* deg/s physical vertical-rate clip */
#define EL_CAP_DEG 1.5     /* deg cap on the vertical box half-height */

#define FUSE_MAXTRK 128
#define FUSE_HIST   48     /* samples retained per track (>= EL_WMAX * frame rate) */

typedef struct {
    int    tid;
    int    used;
    double last_seen;
    double last_t;         /* for the rate limit */
    float  last_el;
    int    n, head;        /* ring fill / write index */
    double th[FUSE_HIST];
    float  eh[FUSE_HIST];
} ElTrk;

struct Fuse { ElTrk trk[FUSE_MAXTRK]; };

Fuse *fuse_new(void) { return (Fuse *)calloc(1, sizeof(Fuse)); }
void  fuse_free(Fuse *f) { free(f); }

static inline float clampf(float x, float lo, float hi){ return x<lo?lo:(x>hi?hi:x); }
static inline float t_range(const RadarTarget *t){ return sqrtf(t->x*t->x + t->y*t->y); }
static inline float t_az(const RadarTarget *t){ return (float)(atan2(t->x, t->y) / DEG); }
static inline float t_el(const RadarTarget *t){
    float h = sqrtf(t->x*t->x + t->y*t->y);
    return (float)(atan2(t->z, h) / DEG);
}

/* find (or allocate, LRU-evicting) the elevation-history slot for a track id */
static ElTrk *slot_for(Fuse *f, int tid, double now) {
    ElTrk *lru = &f->trk[0];
    for (int i = 0; i < FUSE_MAXTRK; i++) {
        ElTrk *e = &f->trk[i];
        if (e->used && e->tid == tid) return e;
        if (!e->used) { lru = e; break; }
        if (e->last_seen < lru->last_seen) lru = e;
    }
    memset(lru, 0, sizeof(*lru));
    lru->used = 1; lru->tid = tid;
    (void)now;
    return lru;
}

static int cmp_f(const void *a, const void *b) {
    float d = *(const float *)a - *(const float *)b;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}

/* trailing-median elevation for one target; updates the track's history */
static float smooth_el(Fuse *f, const RadarTarget *t, double now) {
    float r  = t_range(t);
    float el = t_el(t);
    ElTrk *e = slot_for(f, t->tid, now);

    /* physical rate limit before the sample enters the window */
    if (e->n > 0) {
        double dt = now - e->last_t; if (dt < 1e-3) dt = 1e-3;
        float lim = (float)(EL_MAXRATE * dt);
        if (fabsf(el - e->last_el) > lim)
            el = e->last_el + (el > e->last_el ? lim : -lim);
    }
    /* push */
    e->th[e->head] = now; e->eh[e->head] = el;
    e->head = (e->head + 1) % FUSE_HIST;
    if (e->n < FUSE_HIST) e->n++;

    /* median over the range-scaled trailing window */
    double w = clampf((float)(EL_BASE * (r / 200.0)), EL_WMIN, EL_WMAX);
    float buf[FUSE_HIST]; int m = 0;
    for (int i = 0; i < e->n; i++)
        if (now - e->th[i] <= w) buf[m++] = e->eh[i];
    float out;
    if (m == 0) out = el;
    else { qsort(buf, m, sizeof(float), cmp_f); out = buf[m/2]; }

    e->last_t = now; e->last_el = out; e->last_seen = now;
    return out;
}

int fuse_step(Fuse *f, const RadarTarget *F, int nF,
              const RadarTarget *S, int nS, double now_s,
              RadarTarget *out, int max_out) {
    int nout = 0;
    for (int i = 0; i < nF && nout < max_out; i++) out[nout++] = F[i];

    /* S only where no already-emitted box shares its range+bearing */
    for (int i = 0; i < nS && nout < max_out; i++) {
        float rs = t_range(&S[i]), as = t_az(&S[i]);
        int hit = 0;
        for (int o = 0; o < nout; o++)
            if (fabsf(t_range(&out[o]) - rs) < MATCH_R && fabsf(t_az(&out[o]) - as) < MATCH_A) { hit = 1; break; }
        if (!hit) out[nout++] = S[i];
    }

    /* condition elevation of every published target (F and S alike) */
    for (int o = 0; o < nout; o++) {
        RadarTarget *t = &out[o];
        float h = sqrtf(t->x*t->x + t->y*t->y);
        float el_s = smooth_el(f, t, now_s);
        t->z = h * tanf(el_s * (float)DEG);              /* reposition vertically */

        float cap_m = h * tanf(EL_CAP_DEG * (float)DEG); /* cap the box height */
        if (t->sz > cap_m) t->sz = cap_m;
        if (t->sz < 0.5f)  t->sz = 0.5f;
    }
    return nout;
}
