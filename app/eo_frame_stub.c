/* Synthetic EO source — a stand-in for the real EO channel so the GUI builds and
 * streams standalone. A producer thread renders a moving test scene (GRAY8,
 * 1440x1088, ~60 fps) into a small ring and publishes the latest index+seq, exactly
 * mirroring the real channel's zero-copy handoff. Swapping in the real EO channel is
 * a link-time change: drop this file, link the EO agent's implementation of
 * eo_start/eo_stop/eo_get_latest/eo_focal_mm/eo_pixel_um. */
#define _GNU_SOURCE
#include "eo_frame.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STUB_W 1440
#define STUB_H 1088
#define NBUF   3          /* enough that a borrowed frame survives a ~1-frame read */
#define HORIZON (STUB_H * 52 / 100)

static pthread_t       th;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *buf[NBUF];
static uint8_t        *bg = NULL;   /* prebuilt static scene (gradient+terrain+grain) */
static int             latest = -1;
static uint64_t        seqv = 0;
static double          t_cap = 0.0;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint32_t rng_state = 0x1234567u;
static inline uint32_t xr(void)      /* cheap xorshift PRNG for sensor grain */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Build a realistic thermal-ish scene ONCE: cool sky, a horizon haze band, warmer
 * textured ground with a few heat lumps, a vignette, and fine sensor grain. The
 * per-frame render copies this and only paints the moving target — cheap + alive. */
static void build_bg(void)
{
    const double cxp = STUB_W / 2.0, cyp = STUB_H / 2.0;
    const double maxd = sqrt(cxp * cxp + cyp * cyp);
    for (int y = 0; y < STUB_H; y++) {
        uint8_t *row = bg + (size_t)y * STUB_W;
        for (int x = 0; x < STUB_W; x++) {
            double base;
            if (y < HORIZON) {                        /* sky: cool, darker up high */
                double f = (double)y / HORIZON;
                base = 26 + 30 * f;                    /* 26 -> 56 toward horizon   */
            } else {                                   /* ground: warmer, textured  */
                double f = (double)(y - HORIZON) / (STUB_H - HORIZON);
                base = 70 + 34 * f;                    /* 70 -> 104                 */
                /* rolling terrain heat lumps */
                base += 14 * sin(x * 0.012 + y * 0.02)
                      + 9  * sin(x * 0.03 - y * 0.015);
            }
            if (y >= HORIZON - 4 && y <= HORIZON + 2) base += 26;   /* haze band */
            /* vignette */
            double dx = x - cxp, dy = y - cyp;
            double vig = 1.0 - 0.45 * (sqrt(dx * dx + dy * dy) / maxd);
            base *= vig;
            /* fine grain */
            base += (int)(xr() & 15) - 7;
            row[x] = (uint8_t)clamp8((int)base);
        }
    }
}

/* Additive soft hot blob (target / heat source). */
static void hot_blob(uint8_t *p, int cx, int cy, int rad, int peak)
{
    for (int y = -rad; y <= rad; y++) {
        int yy = cy + y; if (yy < 0 || yy >= STUB_H) continue;
        for (int x = -rad; x <= rad; x++) {
            int xx = cx + x; if (xx < 0 || xx >= STUB_W) continue;
            double d2 = (double)(x * x + y * y) / (double)(rad * rad);
            if (d2 > 1.0) continue;
            int add = (int)(peak * (1.0 - d2) * (1.0 - d2));
            size_t i = (size_t)yy * STUB_W + xx;
            p[i] = (uint8_t)clamp8(p[i] + add);
        }
    }
}

static void render(uint8_t *p, double t)
{
    memcpy(p, bg, (size_t)STUB_W * STUB_H);          /* static scene */

    /* light time-varying shimmer near the horizon (heat haze) */
    int hy = HORIZON;
    for (int x = 0; x < STUB_W; x += 3) {
        int j = (int)(3 * sin(x * 0.05 + t * 2.0));
        int yy = hy + j; if (yy < 0 || yy >= STUB_H) continue;
        size_t i = (size_t)yy * STUB_W + x;
        p[i] = (uint8_t)clamp8(p[i] + 18);
    }

    /* primary moving target — a warm blob drifting along the ground line */
    int tx = (int)(STUB_W * (0.5 + 0.30 * sin(t * 0.22)));
    int ty = (int)(HORIZON + 60 + 26 * sin(t * 0.5));
    hot_blob(p, tx, ty, 46, 70);                     /* soft halo */
    hot_blob(p, tx, ty, 16, 150);                    /* hot core  */

    /* a second, fainter contact */
    int sx = (int)(STUB_W * (0.34 + 0.06 * sin(t * 0.15)));
    int sy = HORIZON + 150;
    hot_blob(p, sx, sy, 26, 45);
}

static void *producer(void *a)
{
    (void)a;
    double t0 = now_s();
    while (run_flag) {
        int nb = (latest + 1) % NBUF;
        render(buf[nb], now_s() - t0);
        pthread_mutex_lock(&lk);
        latest = nb;
        seqv++;
        t_cap = now_s();
        pthread_mutex_unlock(&lk);
        struct timespec ts = { 0, 16000000L };   /* ~60 fps */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int eo_start(const char *dev)
{
    (void)dev;                                    /* stub has no camera */
    bg = malloc((size_t)STUB_W * STUB_H);
    if (!bg) return -1;
    build_bg();
    for (int i = 0; i < NBUF; i++) {
        buf[i] = malloc((size_t)STUB_W * STUB_H);
        if (!buf[i]) return -1;
        memset(buf[i], 0, (size_t)STUB_W * STUB_H);
    }
    run_flag = 1;
    if (pthread_create(&th, NULL, producer, NULL) != 0) {
        run_flag = 0;
        return -1;
    }
    fprintf(stderr, "eo: STUB synthetic source %dx%d ~60 fps (no camera)\n", STUB_W, STUB_H);
    return 0;
}

void eo_stop(void)
{
    if (run_flag) {
        run_flag = 0;
        pthread_join(th, NULL);
    }
    for (int i = 0; i < NBUF; i++) {
        free(buf[i]);
        buf[i] = NULL;
    }
    free(bg);
    bg = NULL;
}

int eo_get_latest(eo_frame_t *out)
{
    pthread_mutex_lock(&lk);
    int idx = latest;
    uint64_t s = seqv;
    double tc = t_cap;
    pthread_mutex_unlock(&lk);
    if (idx < 0) return 0;
    out->data      = buf[idx];
    out->seq       = s;
    out->width     = STUB_W;
    out->height    = STUB_H;
    out->stride    = STUB_W;
    out->fmt       = EO_FMT_GRAY8;
    out->t_capture = tc;
    return 1;
}

double eo_focal_mm(void) { return 12.0; }   /* CommonLands CIL122 f=12mm */
double eo_pixel_um(void) { return 3.45; }   /* IMX296 3.45um pixel       */
