/* EO source — implements the eo_frame.h handoff on top of the eo/pipeline capture +
 * AE + ISP datapath (the EO module: IMX296 mono visible camera). A capture thread
 * dequeues Y10 frames from V4L2, runs the flicker-free AE, tone-maps to 8-bit, and
 * publishes the latest display frame to a small ring. If the camera isn't present,
 * eo_start() fails cleanly, eo_connected() stays 0, and the GUI shows "NOT CONNECTED"
 * (there is no synthetic fallback).
 *
 * Lane note: this reuses the EO module's capture/ae/isp/sensor as-is (links their
 * objects) — it does not reimplement or "optimize" them; that stays the EO datapath's
 * job. If those internals are refactored, this provider tracks the pipeline.h API. */
#define _GNU_SOURCE
#include "eo_frame.h"
#include "pipeline.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NBUF 3

static pthread_t       th;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *out[NBUF];      /* tone-mapped 8-bit display frames */
static int             W = 0, H = 0;
static int             latest = -1;
static uint64_t        seqv = 0;
static double          t_cap = 0.0;
static Sensor          sensor;
static Capture         cap;
static int             g_open = 0;         /* 1 once the camera opened successfully */

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Capture loop — minimal, so it holds the sensor's 60 fps. ONE streaming copy out of
 * the uncached V4L2 DMA buffer, requeue immediately, AE metering at ~15 Hz, publish
 * the RAW Y10 frame. No tone-map/scale here — that is display work and runs on the
 * GUI's rate-capped encoder thread (isp_scale_tonemap), decoupled from capture. */
static void *capture_thread(void *a)
{
    (void)a;
    AE ae;
    ae_init(&ae);
    sensor_apply(&sensor, ae.exp_lines, ae.gain);

    unsigned long n = 0;
    while (run_flag) {
        int idx;
        const uint8_t *raw = cap_dqbuf(&cap, &idx);
        if (!raw) break;
        int nb = (latest + 1) % NBUF;            /* only this thread writes `latest` */
        memcpy(out[nb], raw, cap.sizeimage);     /* the one mandatory copy */
        cap_requeue(&cap, idx);

        if (++n % 4 == 0) {                       /* AE @ ~15 Hz (subsampled, cheap) */
            double mean = isp_mean10(out[nb], cap.bytesperline, cap.width, cap.height);
            ae_update(&ae, mean);
            sensor_apply(&sensor, ae.exp_lines, ae.gain);
        }

        pthread_mutex_lock(&lk);
        latest = nb; seqv++; t_cap = now_s();
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

int eo_start(const char *dev)
{
    const char *d = (dev && *dev) ? dev : EO_DEV_DEFAULT;
    if (sensor_open(&sensor) < 0) {
        fprintf(stderr, "eo: sensor_open failed (i2c *-001a not found)\n");
        return -1;
    }
    if (cap_open(&cap, d, 8) < 0) {
        fprintf(stderr, "eo: cap_open(%s) failed\n", d);
        sensor_close(&sensor);
        return -1;
    }
    W = cap.width; H = cap.height;
    for (int i = 0; i < NBUF; i++) {          /* raw Y10 frames (sizeimage each) */
        out[i] = malloc((size_t)cap.sizeimage);
        if (!out[i]) return -1;
        memset(out[i], 0, (size_t)cap.sizeimage);
    }
    run_flag = 1;
    if (pthread_create(&th, NULL, capture_thread, NULL) != 0) { run_flag = 0; return -1; }
    g_open = 1;
    fprintf(stderr, "eo: V4L2 %s %dx%d (real camera)\n", d, W, H);
    return 0;
}

void eo_stop(void)
{
    if (!g_open) return;                     /* never started (no camera) — nothing to close */
    if (run_flag) { run_flag = 0; pthread_join(th, NULL); }
    cap_close(&cap);
    sensor_close(&sensor);
    for (int i = 0; i < NBUF; i++) { free(out[i]); out[i] = NULL; }
    g_open = 0;
}

/* 1 once the camera is open and has produced at least one frame; 0 if absent. */
int eo_connected(void) { return g_open && latest >= 0; }

int eo_get_latest(eo_frame_t *o)
{
    pthread_mutex_lock(&lk);
    int idx = latest; uint64_t s = seqv; double tc = t_cap;
    pthread_mutex_unlock(&lk);
    if (idx < 0) return 0;
    o->data = out[idx]; o->seq = s; o->width = W; o->height = H;
    o->stride = cap.bytesperline; o->fmt = EO_FMT_Y10; o->t_capture = tc;
    return 1;
}

double eo_focal_mm(void) { return EO_FOCAL_MM; }
double eo_pixel_um(void) { return EO_PIX_UM; }
