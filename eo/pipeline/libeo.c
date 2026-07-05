/* libeo — the EO module core. Owns the camera and the capture -> AE loop, and hands the
 * RAW full-native Y10 frame to consumers (the detector) via the frozen eo.h API.
 *
 * ONE thread. Display processing — tone-map, downscale, median — is the DISPLAY
 * consumer's job, done at the DISPLAY resolution (see main.c). So we never tone-map
 * 1.5M pixels just to shrink them; the cost scales with what's shown, not the sensor.
 * The detector gets the raw frame untouched. Bench controls live in eo_bench.h. */
#include "eo.h"
#include "eo_bench.h"
#include "pipeline.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---- module state ---- */
static Sensor    g_sensor;
static Capture   g_cap;
static AE        g_ae;
static pthread_t g_cap_th;
static volatile int g_run = 0;
static volatile int g_connected = 0;

/* exposure/gain override (bench). Defaults = auto. */
static volatile int g_ae_on    = 1;
static volatile int g_man_gain = 40;
static volatile int g_man_exp  = EO_MAX_EXP_LINES;   /* ~16.5 ms */
static volatile int g_man_vmax = EO_VMAX_MIN;        /* 60 fps   */
static volatile int g_gaincap  = EO_GAIN_CAP;
static volatile int g_median   = 1;   /* ON by default (operator requirement). Runs inside the
                                       * encode workers, so it parallelizes — 60 fps holds. */
static double       g_focus    = 0.0;

/* raw full-native frame -> consumer (triple buffer = zero-copy latest) */
static struct {
    pthread_mutex_t lock;
    uint8_t *buf[3];
    int      w, h, stride;      /* stride = bytesperline (Y10 = 2 bytes/px) */
    int      front;             /* newest ready buffer, -1 = none        */
    int      inuse;             /* buffer currently held by a consumer   */
    unsigned long seq;
} F;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Detector hook — receives the linear Y10 frame (stride = bpl). Stub; a real detector
 * copies out here without blocking capture. */
static void consume_frame(const uint8_t *y10, int bpl, int w, int h)
{
    (void)y10; (void)bpl; (void)w; (void)h;
}

/* ---- capture thread: sensor rate, native Y10 -> triple buffer ---- */
static void *cap_thread(void *arg)
{
    (void)arg;
    unsigned long n = 0;
    while (g_run) {
        /* pick a buffer that isn't the published front or the one a consumer holds */
        pthread_mutex_lock(&F.lock);
        int wi = 0; while (wi == F.front || wi == F.inuse) wi++;
        pthread_mutex_unlock(&F.lock);

        int idx;
        const uint8_t *raw = cap_dqbuf(&g_cap, &idx);
        if (!raw) break;
        memcpy(F.buf[wi], raw, g_cap.sizeimage);   /* one streaming copy off uncached DMA */
        cap_requeue(&g_cap, idx);
        g_connected = 1;

        int bpl = g_cap.bytesperline, w = g_cap.width, h = g_cap.height;
        consume_frame(F.buf[wi], bpl, w, h);        /* detector, raw Y10 (cached copy) */

        if (++n % 4 == 0) {                          /* AE + focus @ ~15 Hz */
            double mean = isp_mean10(F.buf[wi], bpl, w, h);
            g_ae.vmax = g_man_vmax;                   /* fixed operating fps (never auto-changed) */
            if (g_ae_on) {
                ae_update(&g_ae, mean, g_gaincap);
            } else {
                int cap = g_man_vmax - EO_SHS1_MIN;
                g_ae.exp_lines = g_man_exp > cap ? cap : g_man_exp;
                g_ae.gain = g_man_gain; g_ae.mean = mean;
            }
            sensor_apply(&g_sensor, g_ae.exp_lines, g_ae.gain, g_ae.vmax);
            g_focus = isp_sharpness(F.buf[wi], bpl, w, h);
        }

        pthread_mutex_lock(&F.lock);
        F.w = w; F.h = h; F.stride = bpl; F.front = wi; F.seq++;
        pthread_mutex_unlock(&F.lock);
    }
    return NULL;
}

/* ---- frozen API ---- */
int eo_start(const char *dev)
{
    if (g_run) return 0;
    if (!dev) dev = EO_DEV_DEFAULT;
    if (sensor_open(&g_sensor) < 0) return -1;
    if (cap_open(&g_cap, dev, 8) < 0) { sensor_close(&g_sensor); return -1; }

    ae_init(&g_ae);
    sensor_apply(&g_sensor, g_ae.exp_lines, g_ae.gain, g_ae.vmax);

    pthread_mutex_init(&F.lock, NULL);
    for (int i = 0; i < 3; i++) F.buf[i] = malloc((size_t)g_cap.sizeimage);
    F.front = -1; F.inuse = -1; F.seq = 0;
    if (!F.buf[0] || !F.buf[1] || !F.buf[2]) { eo_stop(); return -1; }

    g_run = 1;
    pthread_create(&g_cap_th, NULL, cap_thread, NULL);
    return 0;
}

void eo_stop(void)
{
    if (g_run) { g_run = 0; pthread_join(g_cap_th, NULL); }
    g_connected = 0;
    cap_close(&g_cap);
    sensor_close(&g_sensor);
    for (int i = 0; i < 3; i++) { free(F.buf[i]); F.buf[i] = NULL; }
}

int eo_latest(const uint8_t **buf, uint64_t *seq, int *w, int *h, int *stride, int *fmt)
{
    pthread_mutex_lock(&F.lock);
    if (F.front < 0) { pthread_mutex_unlock(&F.lock); return 0; }
    F.inuse = F.front;
    if (buf)    *buf    = F.buf[F.inuse];
    if (seq)    *seq    = F.seq;
    if (w)      *w      = F.w;
    if (h)      *h      = F.h;
    if (stride) *stride = F.stride;
    if (fmt)    *fmt    = EO_FMT_Y10;
    pthread_mutex_unlock(&F.lock);
    return 1;
}

int    eo_connected(void) { return g_connected; }
double eo_focal_mm(void)  { return EO_FOCAL_MM; }
double eo_pixel_um(void)  { return EO_PIX_UM; }

/* ---- bench controls ---- */
void eo_stats(EoStats *o)
{
    if (!o) return;
    int vm = g_ae.vmax < EO_VMAX_MIN ? EO_VMAX_MIN : g_ae.vmax;
    o->fps = 0.0;   /* wire fps is measured by the emitter (mjpeg.c) */
    o->sfps = EO_FPS_OF_VMAX(vm); o->mean = g_ae.mean;
    o->exp_ms = EO_EXP_US(g_ae.exp_lines) / 1000.0; o->duty_pct = EO_DUTY_PCT(g_ae.exp_lines, vm);
    o->gain = g_ae.gain; o->vmax = vm; o->ae_on = g_ae_on; o->gaincap = g_gaincap;
    o->median = g_median; o->focus = g_focus; o->connected = g_connected;
}

void eo_set_ae(int on)       { g_ae_on = on ? 1 : 0; }
void eo_set_gain(int g)      { g_man_gain = clampi(g, 0, EO_GAIN_MAX); g_ae_on = 0; }
void eo_set_gaincap(int c)   { g_gaincap = clampi(c, 0, EO_GAIN_MAX); }
void eo_set_median(int on)   { g_median = on ? 1 : 0; }
int  eo_median_on(void)      { return g_median; }

/* Operating frame rate — the FIXED fps that caps exposure. The AE never changes it;
 * lowering it is how the operator buys exposure headroom for dark scenes. The sensor
 * runs at this rate, so it also sets the emit/stream rate. */
void eo_set_fps(double fps)
{
    if (fps < 1.0) fps = 1.0;
    g_man_vmax = clampi(EO_VMAX_OF_FPS(fps), EO_VMAX_MIN, EO_VMAX_MAX);
    int cap = g_man_vmax - EO_SHS1_MIN;              /* keep manual exposure within it */
    if (g_man_exp > cap) g_man_exp = cap;
}

/* Manual exposure, capped by the current operating fps (does NOT change the fps). */
void eo_set_expms(double ms)
{
    int l = (int)(ms * 1000.0 / EO_LINE_US + 0.5);
    g_man_exp = clampi(l, EO_MIN_EXP_LINES, g_man_vmax - EO_SHS1_MIN);
    g_ae_on = 0;
}
