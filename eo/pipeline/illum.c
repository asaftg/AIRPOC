/* SG-IR850 illuminator shim. TWO locks by design (WP02 Major 5):
 *   - `lock`  guards the blocking serial device (power/zoom moves over a 9600-baud UART).
 *   - `slock` guards the small cached state that illum_snapshot() returns.
 * The capture loop calls illum_snapshot() twice per frame (libeo cap_thread) — it must
 * NEVER wait on a serial transaction, so snapshot takes only the fast `slock`. Setters
 * do their serial I/O under `lock`, then publish the new state under `slock`. Lock order
 * is always lock -> slock (never the reverse), and snapshot never takes `lock`, so an
 * operator zoom command can't block the capture thread and drop frames. */
#include "illum.h"
#include "sg_ir850.h"
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock  = PTHREAD_MUTEX_INITIALIZER;  /* serial device (blocking) */
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;  /* cached state (fast reads) */
static sg_ir850_t dev = { .fd = -1 };
static int    g_ok    = 0;               /* device present? (set once at start)  */
static int    g_on    = 0;
static int    g_power = 64;              /* commanded drive level (~25% default) */
static double g_fov   = SG_FOV_MAX_DEG;  /* widest flood to start                */

int illum_start(const char *port)
{
    pthread_mutex_lock(&lock);
    int ok = (sg_open(&dev, port, 0) == 0);
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&slock);
    g_ok = ok;
    pthread_mutex_unlock(&slock);
    fprintf(stderr, ok ? "illum: %s opened\n"
                       : "illum: %s not present — illuminator controls disabled\n", port);
    return ok;
}

void illum_set_on(int on)
{
    pthread_mutex_lock(&slock); int ok = g_ok, pwr = g_power; pthread_mutex_unlock(&slock);
    if (!ok) return;
    on = on ? 1 : 0;
    pthread_mutex_lock(&lock);
    sg_power(&dev, on ? true : false);
    /* the device forces full drive on every power-on; restore the commanded level */
    if (on) sg_set_power(&dev, (uint8_t)pwr);
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&slock); g_on = on; pthread_mutex_unlock(&slock);
}

void illum_set_power(int level)
{
    if (level < 0)   level = 0;
    if (level > 255) level = 255;
    pthread_mutex_lock(&slock); g_power = level; int ok = g_ok, on = g_on; pthread_mutex_unlock(&slock);
    if (ok && on) {
        pthread_mutex_lock(&lock);
        sg_set_power(&dev, (uint8_t)level);
        pthread_mutex_unlock(&lock);
    }
}

void illum_set_fov(double deg)
{
    if (deg < SG_FOV_MIN_DEG) deg = SG_FOV_MIN_DEG;
    if (deg > SG_FOV_MAX_DEG) deg = SG_FOV_MAX_DEG;
    pthread_mutex_lock(&slock); g_fov = deg; int ok = g_ok; pthread_mutex_unlock(&slock);
    if (ok) {
        pthread_mutex_lock(&lock);
        sg_set_fov_deg(&dev, deg);
        pthread_mutex_unlock(&lock);
    }
}

/* Fast, serial-free read for the capture loop and /stats — takes only `slock`. */
void illum_snapshot(int *on, int *power, double *fov, int *present)
{
    pthread_mutex_lock(&slock);
    if (on)      *on = g_on;
    if (power)   *power = g_power;
    if (fov)     *fov = g_fov;
    if (present) *present = g_ok;
    pthread_mutex_unlock(&slock);
}
