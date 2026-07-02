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

static pthread_t       th;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *buf[NBUF];
static int             latest = -1;
static uint64_t        seqv = 0;
static double          t_cap = 0.0;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void render(uint8_t *p, double t)
{
    /* target drifts across the scene so motion/latency are visible */
    int cx = (int)(STUB_W * (0.5 + 0.32 * sin(t * 0.6)));
    int cy = (int)(STUB_H * (0.45 + 0.14 * sin(t * 0.9)));

    for (int y = 0; y < STUB_H; y++) {           /* vertical gradient background */
        uint8_t base = (uint8_t)(18 + (y * 34) / STUB_H);
        memset(p + (size_t)y * STUB_W, base, STUB_W);
    }
    int hy = STUB_H * 55 / 100;                   /* horizon line */
    memset(p + (size_t)hy * STUB_W, 64, STUB_W);

    int bw = 90, bh = 120, x0 = cx - bw / 2, y0 = cy - bh / 2;   /* target box */
    for (int x = 0; x < bw; x++) {
        int xx = x0 + x;
        if (xx < 0 || xx >= STUB_W) continue;
        if (y0 >= 0 && y0 < STUB_H)         p[(size_t)y0 * STUB_W + xx] = 235;
        int yb = y0 + bh - 1;
        if (yb >= 0 && yb < STUB_H)         p[(size_t)yb * STUB_W + xx] = 235;
    }
    for (int y = 0; y < bh; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= STUB_H) continue;
        if (x0 >= 0 && x0 < STUB_W)         p[(size_t)yy * STUB_W + x0] = 235;
        int xr = x0 + bw - 1;
        if (xr >= 0 && xr < STUB_W)         p[(size_t)yy * STUB_W + xr] = 235;
    }
    p[(size_t)(STUB_H / 2) * STUB_W + STUB_W / 2] = 255;  /* center marker */
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
