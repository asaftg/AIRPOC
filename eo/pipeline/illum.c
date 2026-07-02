/* SG-IR850 illuminator shim — guards the single serial handle with a mutex (the
 * monitor server calls these from concurrent /ctl client threads) and caches the
 * commanded state for /stats. Never called from the capture loop (serial blocks). */
#include "illum.h"
#include "sg_ir850.h"
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static sg_ir850_t dev = { .fd = -1 };
static int    g_ok    = 0;               /* device present?                     */
static int    g_on    = 0;
static int    g_power = 64;              /* commanded drive level (~25% default) */
static double g_fov   = SG_FOV_MAX_DEG;  /* widest flood to start                */

int illum_start(const char *port)
{
    pthread_mutex_lock(&lock);
    if (sg_open(&dev, port, 0) == 0) {
        g_ok = 1;
        fprintf(stderr, "illum: %s opened\n", port);
    } else {
        fprintf(stderr, "illum: %s not present — illuminator controls disabled\n", port);
    }
    pthread_mutex_unlock(&lock);
    return g_ok;
}

void illum_set_on(int on)
{
    pthread_mutex_lock(&lock);
    if (g_ok) {
        sg_power(&dev, on ? true : false);
        g_on = on ? 1 : 0;
        /* the device forces full drive on every power-on; restore the commanded level */
        if (on) sg_set_power(&dev, (uint8_t)g_power);
    }
    pthread_mutex_unlock(&lock);
}

void illum_set_power(int level)
{
    if (level < 0)   level = 0;
    if (level > 255) level = 255;
    pthread_mutex_lock(&lock);
    g_power = level;
    if (g_ok && g_on) sg_set_power(&dev, (uint8_t)level);
    pthread_mutex_unlock(&lock);
}

void illum_set_fov(double deg)
{
    if (deg < SG_FOV_MIN_DEG) deg = SG_FOV_MIN_DEG;
    if (deg > SG_FOV_MAX_DEG) deg = SG_FOV_MAX_DEG;
    pthread_mutex_lock(&lock);
    g_fov = deg;
    if (g_ok) sg_set_fov_deg(&dev, deg);
    pthread_mutex_unlock(&lock);
}

void illum_snapshot(int *on, int *power, double *fov, int *present)
{
    pthread_mutex_lock(&lock);
    if (on)      *on = g_on;
    if (power)   *power = g_power;
    if (fov)     *fov = g_fov;
    if (present) *present = g_ok;
    pthread_mutex_unlock(&lock);
}
