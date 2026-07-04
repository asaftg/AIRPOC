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

/* Zoom + 4:3 crop + area-average scale, in one pass: take the centred zoom region
 * (sw/z x sh/z) of the finished 8-bit frame, crop it to the output's 4:3 aspect, then
 * box-average it down (or nearest-up) to the selected display size dw x dh. All four
 * display sizes are 4:3, so the GUI's video box never changes shape. The area-average
 * on downscale also knocks noise down; z=1 native is a near-1:1 copy. */
static void display_scale8(const uint8_t *in, int sw, int sh, int z,
                           uint8_t *out, int dw, int dh)
{
    if (z < 1) z = 1;
    int rw = sw / z, rh = sh / z;              /* zoom crop region */
    int rh43 = rw * dh / dw;                   /* force region to the 4:3 output aspect */
    if (rh43 > rh) { rh43 = rh; rw = rh43 * dw / dh; }
    int cx = (sw - rw) / 2, cy = (sh - rh43) / 2;
    for (int oy = 0; oy < dh; oy++) {
        int sy0 = cy + oy * rh43 / dh, sy1 = cy + (oy + 1) * rh43 / dh; if (sy1 <= sy0) sy1 = sy0 + 1;
        uint8_t *o = out + (size_t)oy * dw;
        for (int ox = 0; ox < dw; ox++) {
            int sx0 = cx + ox * rw / dw, sx1 = cx + (ox + 1) * rw / dw; if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned acc = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = in + (size_t)sy * sw;
                for (int sx = sx0; sx < sx1; sx++) { acc += row[sx]; cnt++; }
            }
            o[ox] = (uint8_t)(cnt ? acc / cnt : 0);
        }
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
            int tw, th; mjpeg_res_dims(&tw, &th);          /* operator-selected display size */
            if (!disp || tw != dw || th != dh) { free(disp); disp = malloc((size_t)tw * th); dw = tw; dh = th; }
            if (disp) { display_scale8(buf, w, h, mjpeg_zoom(), disp, dw, dh); mjpeg_publish(disp, dw, dh); }
        } else {
            usleep(4000);                      /* poll; libeo produces at the operating fps */
        }
    }

    fprintf(stderr, "eo_pipeline: shutting down\n");
    eo_stop();
    free(disp);
    return 0;
}
