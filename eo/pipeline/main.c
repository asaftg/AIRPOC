/* AIRPOC EO pipeline daemon: capture -> auto-exposure -> ISP -> monitor feed.
 * Production on-device datapath (C), replacing the Python bench preview.
 *
 *   ./eo_pipeline [-d /dev/video0] [-p 8091]
 *
 * The detector plugs in at consume_frame() (currently a stub) and gets the linear
 * 10-bit frame; the MJPEG feed on :8091 is for the operator. */
#include "pipeline.h"
#include <getopt.h>
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

/* The detector hook. Receives the linear Y10 frame (stride = bpl). Stub for now:
 * a real detector reads/copies here without blocking the capture loop. */
static void consume_frame(const uint8_t *y10, int bpl, int w, int h)
{
    (void)y10; (void)bpl; (void)w; (void)h;
}

int main(int argc, char **argv)
{
    const char *dev = EO_DEV_DEFAULT;
    int port = 8091, opt;
    while ((opt = getopt(argc, argv, "d:p:")) != -1) {
        if (opt == 'd') dev = optarg;
        else if (opt == 'p') port = atoi(optarg);
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);          /* dropped MJPEG clients must not kill us */

    Sensor sensor;
    Capture cap;
    if (sensor_open(&sensor) < 0) return 1;
    if (cap_open(&cap, dev, 8) < 0)  return 1;
    mjpeg_start(port);

    AE ae;
    ae_init(&ae);
    sensor_apply(&sensor, ae.exp_lines, ae.gain);

    uint8_t *out8 = malloc((size_t)cap.width * cap.height);
    if (!out8) { perror("malloc"); return 1; }

    fprintf(stderr, "eo_pipeline: %dx%d bpl=%d  monitor http://0.0.0.0:%d/\n",
            cap.width, cap.height, cap.bytesperline, port);

    double fps = 0.0, t_last = now_s();
    unsigned long frame_n = 0;

    while (g_run) {
        int idx;
        const uint8_t *y10 = cap_dqbuf(&cap, &idx);
        if (!y10) break;

        consume_frame(y10, cap.bytesperline, cap.width, cap.height);   /* detector */

        if (++frame_n % 4 == 0) {                                      /* AE @ ~15 Hz */
            double mean = isp_mean10(y10, cap.bytesperline, cap.width, cap.height);
            ae_update(&ae, mean);
            sensor_apply(&sensor, ae.exp_lines, ae.gain);
        }

        isp_tonemap(y10, cap.bytesperline, cap.width, cap.height, out8);

        double t = now_s(), dt = t - t_last; t_last = t;
        if (dt > 0) { double inst = 1.0 / dt; fps = fps ? 0.85 * fps + 0.15 * inst : inst; }

        mjpeg_publish(out8, cap.width, cap.height, fps, ae.mean, ae.exp_lines, ae.gain);
        cap_requeue(&cap, idx);
    }

    fprintf(stderr, "eo_pipeline: shutting down\n");
    free(out8);
    cap_close(&cap);
    sensor_close(&sensor);
    return 0;
}
