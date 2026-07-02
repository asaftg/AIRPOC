/* Synthetic radar source — a stand-in for the real AWR module so the operator scope
 * renders live returns (a moving track + clutter inside the sector) until the radar
 * module lands. A producer thread updates the frame at ~10 Hz. Replace at link time
 * with the real reader implementing radar.h. */
#define _GNU_SOURCE
#include "radar.h"
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_RANGE_M 500.0f
#define FOV_HALF    60.0f

static pthread_t       th;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static radar_frame_t   cur;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint32_t rs = 0x9e3779b9u;
static float frand(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs & 0xffffff) / (float)0xffffff; }

/* Two moving contacts + doppler-scattered clutter, all inside the ±FOV sector. */
static void gen(radar_frame_t *f, double t)
{
    f->connected   = 1;
    f->max_range_m = MAX_RANGE_M;
    f->fov_half_deg = FOV_HALF;

    /* contact A: inbound, weaving across boresight */
    double azA = 26.0 * sin(t * 0.10) * M_PI / 180.0;
    double rA  = 300.0 - 40.0 * sin(t * 0.20) - 8.0 * t; rA = fmod(rA, 260.0) + 90.0;
    /* contact B: slower, off to one side */
    double azB = (-20.0 + 6.0 * sin(t * 0.07)) * M_PI / 180.0;
    double rB  = 360.0 + 30.0 * sin(t * 0.05);

    radar_target_t T[2];
    T[0] = (radar_target_t){ 1, (float)(rA * sin(azA)), (float)(rA * cos(azA)),
                              (float)(-6.0 * cos(azA)), (float)(-6.0 * sin(azA)),
                              3.0f, 4.0f, 0.82f };
    T[1] = (radar_target_t){ 2, (float)(rB * sin(azB)), (float)(rB * cos(azB)),
                              -1.5f, -0.8f, 4.0f, 3.0f, 0.61f };
    f->num_targets = 2;
    memcpy(f->targets, T, sizeof(T));

    int np = 0;
    /* tight clusters around each contact (the returns the clusterer boxes) */
    for (int ti = 0; ti < 2 && np < RADAR_MAX_POINTS; ti++) {
        for (int k = 0; k < 10 && np < RADAR_MAX_POINTS; k++) {
            radar_point_t p;
            p.x = T[ti].x + (frand() - 0.5f) * 8.0f;
            p.y = T[ti].y + (frand() - 0.5f) * 10.0f;
            p.v = (ti == 0 ? 6.0f : 1.5f) + (frand() - 0.5f) * 1.5f; /* inbound (+) */
            p.snr = 22.0f + frand() * 14.0f;
            f->points[np++] = p;
        }
    }
    /* scattered clutter inside the sector */
    while (np < 60 && np < RADAR_MAX_POINTS) {
        double az = (frand() - 0.5f) * 2.0 * FOV_HALF * M_PI / 180.0;
        double r  = 30.0 + frand() * (MAX_RANGE_M - 40.0);
        radar_point_t p;
        p.x = (float)(r * sin(az));
        p.y = (float)(r * cos(az));
        p.v = (frand() - 0.5f) * 0.3f;                 /* near-static clutter */
        p.snr = 12.0f + frand() * 10.0f;
        f->points[np++] = p;
    }
    f->num_points = np;
}

static void *producer(void *a)
{
    (void)a;
    double t0 = now_s();
    radar_frame_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    while (run_flag) {
        gen(&tmp, now_s() - t0);
        pthread_mutex_lock(&lk);
        cur = tmp;
        pthread_mutex_unlock(&lk);
        struct timespec ts = { 0, 100000000L };        /* ~10 Hz */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int radar_start(const char *port)
{
    (void)port;                                        /* stub has no device */
    memset(&cur, 0, sizeof(cur));
    run_flag = 1;
    if (pthread_create(&th, NULL, producer, NULL) != 0) { run_flag = 0; return -1; }
    return 0;
}

void radar_stop(void)
{
    if (run_flag) { run_flag = 0; pthread_join(th, NULL); }
}

int radar_get_latest(radar_frame_t *out)
{
    pthread_mutex_lock(&lk);
    *out = cur;
    pthread_mutex_unlock(&lk);
    return out->connected;
}
