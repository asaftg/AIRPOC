/* AIRPOC EO pipeline daemon: capture -> auto-exposure -> ISP -> monitor feed.
 * Production on-device datapath (C), replacing the Python bench preview.
 *
 *   ./eo_pipeline [-d /dev/video0] [-p 8091]
 *
 * Two threads, decoupled so the wire feed never throttles capture or detection:
 *
 *   capture thread  (runs at the sensor rate, ~60 fps, always full-res native Y10):
 *       dqbuf -> memcpy out of the uncached DMA buffer -> requeue -> consume_frame()
 *       (detector, native 10-bit) -> AE every 4th frame. Publishes the latest raw
 *       frame + AE stats into a small mutex/cond framestore.
 *   encoder thread  (rate-capped to the wire preset, e.g. 15-25 fps):
 *       waits for a fresh frame, then crop(zoom)+downscale+tone-map to the preset
 *       resolution in one fused pass and MJPEG-encodes once (served to all clients).
 *
 * The detector always sees every native frame; the WiFi feed is deliberately light
 * (small resolution, capped fps, tuned JPEG quality) at ALL times, independent of
 * whether anyone is watching. This is the "100% optimized EO output" the GUI consumes. */
#include "pipeline.h"
#include <getopt.h>
#include "illum.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Latest native frame + AE stats, handed from the capture thread to the encoder. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint8_t *raw;               /* newest Y10 frame (sizeimage bytes)  */
    int      bpl, w, h;
    unsigned long seq;          /* bumped on each new frame            */
    double   fps, mean;
    int      exp_lines, gain;
    int      stop;
} FrameStore;

/* The detector hook. Receives the linear Y10 frame (stride = bpl). Stub for now:
 * a real detector reads/copies here without blocking the capture loop. */
static void consume_frame(const uint8_t *y10, int bpl, int w, int h)
{
    (void)y10; (void)bpl; (void)w; (void)h;
}

/* Encoder thread: light wire feed. Rate-capped to the current preset fps; does the
 * only heavy per-frame pixel work (crop+downscale+tone-map+JPEG) on a private copy
 * so the capture thread is never blocked by it. */
static void *encoder_thread(void *arg)
{
    FrameStore *fs = arg;
    uint8_t *local = malloc((size_t)fs->w * fs->h * 2 + 64);       /* private Y10 copy */
    uint8_t *out8  = malloc((size_t)EO_WIDTH * EO_HEIGHT);         /* full-res 8-bit out */
    if (!local || !out8) { perror("malloc(enc)"); return NULL; }
    unsigned long seen = 0;
    double last_pub = 0.0;

    for (;;) {
        pthread_mutex_lock(&fs->lock);
        while (fs->seq == seen && !fs->stop)
            pthread_cond_wait(&fs->cond, &fs->lock);
        if (fs->stop) { pthread_mutex_unlock(&fs->lock); break; }

        /* Rate-cap the encode without copying if we're ahead of schedule. Capture and
         * the detector keep running at the sensor rate regardless. */
        double t = now_s(), min_dt = EO_FEED_FPS > 0 ? 1.0 / EO_FEED_FPS : 0.0;
        if (last_pub > 0 && (t - last_pub) < min_dt) { seen = fs->seq; pthread_mutex_unlock(&fs->lock); continue; }

        int bpl = fs->bpl, w = fs->w, h = fs->h;
        double fps = fs->fps, mean = fs->mean; int el = fs->exp_lines, g = fs->gain;
        memcpy(local, fs->raw, (size_t)fs->bpl * fs->h);
        seen = fs->seq;
        pthread_mutex_unlock(&fs->lock);

        mjpeg_set_sharp(isp_sharpness(local, bpl, w, h));          /* focus, native ROI */

        /* Digital zoom = center crop upscaled back to full frame, so the feed is a
         * constant w*h at every zoom (z=1 is 1:1 native; z>1 magnifies the center). */
        int z = mjpeg_zoom();                                       /* 1/2/4/8          */
        int cw = w / z, ch = h / z, cx = (w - cw) / 2, cy = (h - ch) / 2;
        isp_scale_tonemap(local, bpl, cx, cy, cw, ch, out8, w, h);

        mjpeg_publish(out8, w, h, fps, mean, el, g);
        last_pub = t;
    }
    free(local); free(out8);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *dev = EO_DEV_DEFAULT;
    const char *iport = "/dev/ttyUSB0";   /* SG-IR850 illuminator (optional) */
    int port = 8091, opt;
    while ((opt = getopt(argc, argv, "d:p:i:")) != -1) {
        if (opt == 'd') dev = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 'i') iport = optarg;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);          /* dropped MJPEG clients must not kill us */

    Sensor sensor;
    Capture cap;
    if (sensor_open(&sensor) < 0) return 1;
    if (cap_open(&cap, dev, 8) < 0)  return 1;
    illum_start(iport);                /* optional; no-ops if the illuminator is absent */
    mjpeg_start(port);

    AE ae;
    ae_init(&ae);
    sensor_apply(&sensor, ae.exp_lines, ae.gain);

    uint8_t *frame = malloc((size_t)cap.sizeimage);   /* cached copy of the DMA buffer */
    FrameStore fs = {0};
    fs.raw = malloc((size_t)cap.sizeimage);
    if (!frame || !fs.raw) { perror("malloc"); return 1; }
    pthread_mutex_init(&fs.lock, NULL);
    pthread_cond_init(&fs.cond, NULL);
    fs.bpl = cap.bytesperline; fs.w = cap.width; fs.h = cap.height;

    pthread_t enc;
    pthread_create(&enc, NULL, encoder_thread, &fs);

    fprintf(stderr, "eo_pipeline: %dx%d bpl=%d  monitor http://0.0.0.0:%d/  (capture+encoder threads)\n",
            cap.width, cap.height, cap.bytesperline, port);

    double fps = 0.0, t_last = now_s();
    unsigned long frame_n = 0;

    while (g_run) {
        int idx;
        const uint8_t *raw = cap_dqbuf(&cap, &idx);
        if (!raw) break;
        /* Copy out of the uncached V4L2 DMA buffer in one streaming pass, then
         * release the buffer immediately (keeps the capture queue from starving). */
        memcpy(frame, raw, cap.sizeimage);
        cap_requeue(&cap, idx);

        consume_frame(frame, cap.bytesperline, cap.width, cap.height);  /* detector */

        if (++frame_n % 4 == 0) {                                      /* AE @ ~15 Hz */
            double mean = isp_mean10(frame, cap.bytesperline, cap.width, cap.height);
            ae_update(&ae, mean);
            sensor_apply(&sensor, ae.exp_lines, ae.gain);
        }

        double t = now_s(), dt = t - t_last; t_last = t;
        if (dt > 0) { double inst = 1.0 / dt; fps = fps ? 0.85 * fps + 0.15 * inst : inst; }

        /* Hand the newest native frame to the encoder (it decides whether to use it,
         * rate-capped to the wire preset). Publishing every 2nd frame is plenty for a
         * <=25 fps feed and halves the hand-off memcpy. */
        if (frame_n % 2 == 0) {
            pthread_mutex_lock(&fs.lock);
            memcpy(fs.raw, frame, (size_t)cap.bytesperline * cap.height);
            fs.seq++;
            fs.fps = fps; fs.mean = ae.mean; fs.exp_lines = ae.exp_lines; fs.gain = ae.gain;
            pthread_cond_signal(&fs.cond);
            pthread_mutex_unlock(&fs.lock);
        }
    }

    fprintf(stderr, "eo_pipeline: shutting down\n");
    pthread_mutex_lock(&fs.lock); fs.stop = 1; pthread_cond_signal(&fs.cond); pthread_mutex_unlock(&fs.lock);
    pthread_join(enc, NULL);
    free(frame); free(fs.raw);
    cap_close(&cap);
    sensor_close(&sensor);
    return 0;
}
