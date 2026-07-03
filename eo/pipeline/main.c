/* AIRPOC EO operator preview (bench tool). A thin CONSUMER of the libeo module:
 * libeo (eo.h) owns the camera + capture + AE + ISP and produces finished frames;
 * this process pulls them with eo_latest(), applies digital zoom, and serves them as
 * MJPEG with a control page. It runs the SAME libeo the GUI links, so there is one
 * datapath and one camera owner. In production the GUI owns the camera and this
 * preview is off; when bench-testing, run this and keep the GUI off.
 *
 *   ./eo_pipeline [-d /dev/video0] [-p 8091] [-i /dev/ttyUSB0]
 */
#include "eo.h"
#include "pipeline.h"
#include "illum.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* Digital zoom: center-crop w/z x h/z of the finished 8-bit frame and nearest-neighbour
 * upscale back to w x h, so the feed is a constant size at every zoom. z=1 is a copy. */
static void zoom8(const uint8_t *in, int w, int h, int z, uint8_t *out)
{
    if (z < 1) z = 1;
    int cw = w / z, ch = h / z, cx = (w - cw) / 2, cy = (h - ch) / 2;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = in + (size_t)(cy + y * ch / h) * w + cx;
        uint8_t *o = out + (size_t)y * w;
        for (int x = 0; x < w; x++) o[x] = row[x * cw / w];
    }
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
    signal(SIGPIPE, SIG_IGN);

    if (eo_start(dev) < 0) { fprintf(stderr, "eo_pipeline: eo_start failed (camera busy?)\n"); return 1; }
    illum_start(iport);
    mjpeg_start(port);
    fprintf(stderr, "eo_pipeline: libeo v%d up  preview http://0.0.0.0:%d/\n", EO_API_VERSION, port);

    uint8_t *disp = NULL;
    int dw = 0, dh = 0;
    uint64_t last = 0;

    while (g_run) {
        const uint8_t *buf; uint64_t seq; int w, h, stride, fmt;
        if (eo_latest(&buf, &seq, &w, &h, &stride, &fmt) && seq != last) {
            last = seq;
            if (!disp || w != dw || h != dh) { free(disp); disp = malloc((size_t)w * h); dw = w; dh = h; }
            if (disp) { zoom8(buf, w, h, mjpeg_zoom(), disp); mjpeg_publish(disp, w, h); }
        } else {
            usleep(4000);                      /* ~250 Hz poll; libeo produces at EO_FEED_FPS */
        }
    }

    fprintf(stderr, "eo_pipeline: shutting down\n");
    eo_stop();
    free(disp);
    return 0;
}
