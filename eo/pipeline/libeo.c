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
#include "airpoc_tap.h"     /* vendored copy of recorder/tap/airpoc_tap.h (protocol v1) */
#include "illum.h"          /* illum_snapshot() for the per-frame illuminator stamp     */
#include <pthread.h>
#include <stdio.h>
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

/* Recorder tap (airpoc.eo_y10): the raw pre-ISP native stream. Zero added copies —
 * when the tap is up, the capture thread's one mandatory memcpy off the DMA buffer
 * lands DIRECTLY in a tap slot (16-deep ring), and that same slot pointer is what
 * consume_frame(), the AE metering, and eo_latest() read. If the recorder/shm is
 * absent, tap_create fails once and we fall back to the heap triple buffer —
 * behavior identical to a recorder-less system. */
static AirTap   g_y10_tap;
static uint32_t g_v4l2_drops = 0;   /* cumulative driver-level drops (v4l2 seq gaps) */

/* raw full-native frame -> consumer (zero-copy latest).
 * Tap mode: ptr = the committed tap slot (ring depth 16 = ~266 ms before reuse;
 * an eo_latest holder keeps a pointer for <= one poll interval, so no race).
 * Fallback:  ptr = one of 3 heap buffers with front/inuse rotation. */
static struct {
    pthread_mutex_t lock;
    const uint8_t *ptr;         /* newest published frame                */
    uint8_t *buf[3];            /* fallback buffers (tap unavailable)    */
    int      w, h, stride;      /* stride = bytesperline (Y10 = 2 bytes/px) */
    int      front;             /* fallback: newest ready buffer index   */
    int      inuse;             /* fallback: buffer held by a consumer   */
    unsigned long seq;
} F;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Detector hook — receives the linear Y10 frame (stride = bpl). Stub; a real detector
 * copies out here without blocking capture. */
static void consume_frame(const uint8_t *y10, int bpl, int w, int h)
{
    (void)y10; (void)bpl; (void)w; (void)h;
}

/* ---- capture thread: sensor rate, native Y10 -> tap slot (or fallback buffer) ---- */
static void *cap_thread(void *arg)
{
    (void)arg;
    unsigned long n = 0;
    uint32_t prev_seq = 0; int have_prev = 0;
    while (g_run) {
        /* destination for this frame: a tap slot (zero-copy publish) or a fallback
         * heap buffer that isn't the published front / the one a consumer holds */
        uint8_t *dst; int wi = -1;
        if (g_y10_tap.ok) {
            dst = tap_slot_begin(&g_y10_tap);
        } else {
            pthread_mutex_lock(&F.lock);
            wi = 0; while (wi == F.front || wi == F.inuse) wi++;
            pthread_mutex_unlock(&F.lock);
            dst = F.buf[wi];
        }

        int idx;
        const uint8_t *raw = cap_dqbuf(&g_cap, &idx);
        if (!raw) break;
        memcpy(dst, raw, g_cap.sizeimage);   /* the one streaming copy off uncached DMA */
        cap_requeue(&g_cap, idx);
        g_connected = 1;
        uint64_t t_src = g_cap.last_ts_ns;   /* exposure-referenced driver timestamp */
        uint32_t vseq  = g_cap.last_seq;
        if (have_prev && vseq > prev_seq + 1) g_v4l2_drops += vseq - prev_seq - 1;
        prev_seq = vseq; have_prev = 1;

        int bpl = g_cap.bytesperline, w = g_cap.width, h = g_cap.height;
        consume_frame(dst, bpl, w, h);        /* detector, raw Y10 (cached copy) */

        if (++n % 4 == 0) {                          /* AE + focus @ ~15 Hz */
            double mean = isp_mean10(dst, bpl, w, h);
            g_ae.vmax = g_man_vmax;                   /* fixed operating fps (never auto-changed) */
            if (g_ae_on) {
                ae_update(&g_ae, mean, g_gaincap);
            } else {
                int cap = g_man_vmax - EO_SHS1_MIN;
                g_ae.exp_lines = g_man_exp > cap ? cap : g_man_exp;
                g_ae.gain = g_man_gain; g_ae.mean = mean;
            }
            sensor_apply(&g_sensor, g_ae.exp_lines, g_ae.gain, g_ae.vmax);
            g_focus = isp_sharpness(dst, bpl, w, h);
        }

        /* commit AFTER the AE step so the recorded meta is frame-current */
        if (g_y10_tap.ok) {
            /* meta[4] = per-frame illuminator state (recorder gates on "illum":1 in
             * meta_json). Packing: bit0=laser_on, bit1=present, [15:8]=power 0..255,
             * [25:16]=fov_deg*10 (0..102.3). One cached read (no serial). mean10 dropped
             * from the per-frame slot — it's in the 5 Hz /stats and recomputable from raw. */
            int lon, lpw, lpr; double lfov;
            illum_snapshot(&lon, &lpw, &lfov, &lpr);
            uint32_t illum = (lon ? 1u : 0u) | (lpr ? 2u : 0u)
                           | (((uint32_t)lpw & 0xFFu) << 8)
                           | ((((uint32_t)(lfov * 10.0)) & 0x3FFu) << 16);
            uint32_t meta[TAP_META_WORDS] = {
                vseq, (uint32_t)g_ae.exp_lines, (uint32_t)g_ae.gain,
                (uint32_t)g_ae.vmax, illum, g_v4l2_drops
            };
            tap_slot_commit(&g_y10_tap, (uint32_t)g_cap.sizeimage, t_src, meta, 0);
        }

        pthread_mutex_lock(&F.lock);
        F.ptr = dst; F.w = w; F.h = h; F.stride = bpl;
        if (wi >= 0) F.front = wi;
        F.seq++;
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
    F.ptr = NULL; F.front = -1; F.inuse = -1; F.seq = 0;

    /* Recorder tap: 16 slots x sizeimage. On failure (recorder never installed, shm
     * permissions) log once and run exactly as before on the heap triple buffer. */
    char mj[320];
    snprintf(mj, sizeof mj,
        "{\"name\":\"eo_y10\",\"fmt\":\"Y10 16-bit LE, px=(b[2x]|b[2x+1]<<8)>>6\","
        "\"w\":%d,\"h\":%d,\"stride\":%d,\"illum\":1,"
        "\"meta\":[\"v4l2_seq\",\"exp_lines\",\"gain\",\"vmax\","
        "\"illum:on|present<<1|power<<8|fov10<<16\",\"drops_cum\"]}",
        g_cap.width, g_cap.height, g_cap.bytesperline);
    if (tap_create(&g_y10_tap, "airpoc.eo_y10", 16, (uint32_t)g_cap.sizeimage, mj) < 0)
        fprintf(stderr, "libeo: eo_y10 tap unavailable — running without recording\n");
    if (!g_y10_tap.ok) {
        for (int i = 0; i < 3; i++) F.buf[i] = malloc((size_t)g_cap.sizeimage);
        if (!F.buf[0] || !F.buf[1] || !F.buf[2]) { eo_stop(); return -1; }
    }

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
    tap_destroy(&g_y10_tap);
    for (int i = 0; i < 3; i++) { free(F.buf[i]); F.buf[i] = NULL; }
}

int eo_latest(const uint8_t **buf, uint64_t *seq, int *w, int *h, int *stride, int *fmt)
{
    pthread_mutex_lock(&F.lock);
    if (!F.ptr) { pthread_mutex_unlock(&F.lock); return 0; }
    F.inuse = F.front;              /* fallback-mode hold; -1 (harmless) in tap mode */
    if (buf)    *buf    = F.ptr;
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
