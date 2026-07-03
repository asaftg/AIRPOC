/* libeo — the EO module core. Owns the camera and the capture -> AE -> ISP datapath
 * and produces finished, display-ready 8-bit mono frames behind the frozen eo.h API.
 *
 * Two internal threads (so a consumer's pull rate can never throttle capture or the
 * detector):
 *   capture thread  — 60 fps: dqbuf -> memcpy out of uncached DMA -> requeue ->
 *                     consume_frame() detector -> AE every 4th -> publish raw Y10.
 *   tone thread     — rate-capped: tone-map + median the newest raw -> a triple-
 *                     buffered FINISHED frame that eo_latest() hands out zero-copy.
 *
 * eo.h is frozen; everything in here may change. Bench controls live in eo_bench.h. */
#include "eo.h"
#include "eo_bench.h"
#include "pipeline.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- module state ---- */
static Sensor    g_sensor;
static Capture   g_cap;
static AE        g_ae;
static pthread_t g_cap_th, g_tone_th;
static volatile int g_run = 0;
static volatile int g_connected = 0;

/* exposure/gain override (bench). Defaults = auto. */
static volatile int g_ae_on   = 1;
static volatile int g_man_gain = 40;
static volatile int g_man_exp  = EO_MAX_EXP_LINES;   /* ~16.5 ms */
static volatile int g_man_vmax = EO_VMAX_MIN;        /* 60 fps   */
static volatile int g_gaincap  = EO_GAIN_CAP;
static volatile int g_median   = 1;
static double       g_focus    = 0.0;

/* raw handoff: capture thread -> tone thread */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint8_t *raw;
    int      bpl, w, h;
    unsigned long seq;
    double   fps, mean;
    int      exp_lines, gain, vmax;
    int      stop;
} R;

/* finished handoff: tone thread -> consumer (triple buffer = zero-copy latest) */
static struct {
    pthread_mutex_t lock;
    uint8_t *buf[3];
    int      w, h, stride;
    int      front;              /* newest ready buffer, -1 = none        */
    int      inuse;              /* buffer currently held by a consumer   */
    unsigned long seq;
} F;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Detector hook — receives the linear Y10 frame. Stub; a real detector copies here. */
static void consume_frame(const uint8_t *y10, int bpl, int w, int h)
{
    (void)y10; (void)bpl; (void)w; (void)h;
}

/* ---- capture thread: sensor rate, native Y10 ---- */
static void *cap_thread(void *arg)
{
    (void)arg;
    uint8_t *frame = malloc((size_t)g_cap.sizeimage);
    if (!frame) return NULL;
    double fps = 0.0, t_last = now_s();
    unsigned long n = 0;

    while (g_run) {
        int idx;
        const uint8_t *raw = cap_dqbuf(&g_cap, &idx);
        if (!raw) break;
        memcpy(frame, raw, g_cap.sizeimage);        /* one streaming copy off uncached DMA */
        cap_requeue(&g_cap, idx);
        g_connected = 1;

        consume_frame(frame, g_cap.bytesperline, g_cap.width, g_cap.height);

        if (++n % 4 == 0) {                          /* AE @ ~15 Hz */
            double mean = isp_mean10(frame, g_cap.bytesperline, g_cap.width, g_cap.height);
            if (g_ae_on) {
                ae_update(&g_ae, mean, g_gaincap);
            } else {
                g_ae.exp_lines = g_man_exp; g_ae.gain = g_man_gain;
                g_ae.vmax = g_man_vmax; g_ae.mean = mean;
            }
            sensor_apply(&g_sensor, g_ae.exp_lines, g_ae.gain, g_ae.vmax);
        }

        double t = now_s(), dt = t - t_last; t_last = t;
        if (dt > 0) { double inst = 1.0 / dt; fps = fps ? 0.85 * fps + 0.15 * inst : inst; }

        if (n % 2 == 0) {                            /* hand the newest raw to tone thread */
            pthread_mutex_lock(&R.lock);
            memcpy(R.raw, frame, (size_t)g_cap.bytesperline * g_cap.height);
            R.seq++;
            R.fps = fps; R.mean = g_ae.mean;
            R.exp_lines = g_ae.exp_lines; R.gain = g_ae.gain; R.vmax = g_ae.vmax;
            pthread_cond_signal(&R.cond);
            pthread_mutex_unlock(&R.lock);
        }
    }
    free(frame);
    return NULL;
}

/* ---- tone thread: rate-capped tone-map + median -> finished triple buffer ---- */
static void *tone_thread(void *arg)
{
    (void)arg;
    uint8_t *local = malloc((size_t)R.w * R.h * 2 + 64);   /* private Y10 copy */
    if (!local) return NULL;
    unsigned long seen = 0;
    double last = 0.0;

    for (;;) {
        pthread_mutex_lock(&R.lock);
        while (R.seq == seen && !R.stop) pthread_cond_wait(&R.cond, &R.lock);
        if (R.stop) { pthread_mutex_unlock(&R.lock); break; }
        double t = now_s(), min_dt = EO_FEED_FPS > 0 ? 1.0 / EO_FEED_FPS : 0.0;
        if (last > 0 && (t - last) < min_dt) { seen = R.seq; pthread_mutex_unlock(&R.lock); continue; }
        int bpl = R.bpl, w = R.w, h = R.h;
        memcpy(local, R.raw, (size_t)R.bpl * R.h);
        seen = R.seq;
        pthread_mutex_unlock(&R.lock);

        g_focus = isp_sharpness(local, bpl, w, h);

        /* choose a write buffer that isn't the front or the one a consumer holds */
        pthread_mutex_lock(&F.lock);
        int wi = 0;
        while (wi == F.front || wi == F.inuse) wi++;
        pthread_mutex_unlock(&F.lock);

        isp_scale_tonemap(local, bpl, 0, 0, w, h, F.buf[wi], w, h);   /* full-frame finish */
        if (g_median) isp_median3(F.buf[wi], w, h);

        pthread_mutex_lock(&F.lock);
        F.w = w; F.h = h; F.stride = w; F.front = wi; F.seq++;
        pthread_mutex_unlock(&F.lock);
        last = t;
    }
    free(local);
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

    pthread_mutex_init(&R.lock, NULL); pthread_cond_init(&R.cond, NULL);
    pthread_mutex_init(&F.lock, NULL);
    R.raw = malloc((size_t)g_cap.sizeimage);
    R.bpl = g_cap.bytesperline; R.w = g_cap.width; R.h = g_cap.height;
    R.seq = 0; R.stop = 0;
    for (int i = 0; i < 3; i++) F.buf[i] = malloc((size_t)g_cap.width * g_cap.height);
    F.front = -1; F.inuse = -1; F.seq = 0;
    if (!R.raw || !F.buf[0] || !F.buf[1] || !F.buf[2]) { eo_stop(); return -1; }

    g_run = 1;
    pthread_create(&g_cap_th, NULL, cap_thread, NULL);
    pthread_create(&g_tone_th, NULL, tone_thread, NULL);
    return 0;
}

void eo_stop(void)
{
    if (g_run) {
        g_run = 0;
        pthread_mutex_lock(&R.lock); R.stop = 1; pthread_cond_signal(&R.cond); pthread_mutex_unlock(&R.lock);
        pthread_join(g_cap_th, NULL);
        pthread_join(g_tone_th, NULL);
    }
    g_connected = 0;
    cap_close(&g_cap);
    sensor_close(&g_sensor);
    free(R.raw); R.raw = NULL;
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
    if (fmt)    *fmt    = EO_FMT_GRAY8;
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
    o->fps = R.fps; o->sfps = EO_FPS_OF_VMAX(vm); o->mean = g_ae.mean;
    o->exp_ms = EO_EXP_US(g_ae.exp_lines) / 1000.0; o->duty_pct = EO_DUTY_PCT(g_ae.exp_lines, vm);
    o->gain = g_ae.gain; o->vmax = vm; o->ae_on = g_ae_on; o->gaincap = g_gaincap;
    o->median = g_median; o->focus = g_focus; o->connected = g_connected;
}

void eo_set_ae(int on)       { g_ae_on = on ? 1 : 0; }
void eo_set_gain(int g)      { g_man_gain = clampi(g, 0, EO_GAIN_MAX); g_ae_on = 0; }
void eo_set_gaincap(int c)   { g_gaincap = clampi(c, 0, EO_GAIN_MAX); }
void eo_set_median(int on)   { g_median = on ? 1 : 0; }
void eo_set_expms(double ms)
{
    int l = (int)(ms * 1000.0 / EO_LINE_US + 0.5);
    g_man_exp  = clampi(l, EO_MIN_EXP_LINES, EO_VMAX_MAX - EO_SHS1_MIN);
    g_man_vmax = clampi(g_man_exp + EO_SHS1_MIN, EO_VMAX_MIN, EO_VMAX_MAX);
    g_ae_on = 0;
}
