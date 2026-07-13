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
#include "eo_bench.h"
#include "pipeline.h"
#include "illum.h"
#include "tdn.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* The zoom crop (centred sw/z x sh/z of the raw), forced to the output's 4:3 aspect.
 * Passed to isp_scale_tonemap, which crops + downscales + tone-maps in one pass at the
 * DISPLAY size — so the expensive work scales with what's shown, not the sensor. */
static void zoom_crop_43(int sw, int sh, int z, int dw, int dh,
                         int *cx, int *cy, int *cw, int *ch)
{
    if (z < 1) z = 1;
    int rw = sw / z, rh = sh / z;
    int rh43 = rw * dh / dw;                    /* force region to the 4:3 output aspect */
    if (rh43 > rh) { rh43 = rh; rw = rh43 * dw / dh; }
    *cw = rw; *ch = rh43; *cx = (sw - rw) / 2; *cy = (sh - rh43) / 2;
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

    uint8_t  *disp = NULL;
    uint16_t *dn   = NULL;                     /* denoised native frame (Q10.5, tdn.c) */
    int dw = 0, dh = 0, dnw = 0, dnh = 0;
    uint64_t last = 0;

    while (g_run) {
        const uint8_t *buf; uint64_t seq; int w, h, stride, fmt;
        if (eo_latest(&buf, &seq, &w, &h, &stride, &fmt) && seq != last) {
            last = seq;
            int tw, th; mjpeg_res_dims(&tw, &th);          /* operator-selected display size */
            if (!disp || tw != dw || th != dh) { free(disp); disp = malloc((size_t)tw * th); dw = tw; dh = th; }
            if (disp) {
                /* P0 night denoise (tdn.c) on the RAW native frame, then crop(zoom, 4:3)
                 * + downscale + tone-map straight to display res. DISPLAY-ONLY: the
                 * detector keeps the raw tap. Gated off (day / knob) it costs nothing
                 * and the path is byte-identical to before. Median + JPEG happen in the
                 * encode worker pool (mjpeg.c), so this loop always holds the frame rate. */
                if (!dn || w != dnw || h != dnh) { free(dn); dn = malloc((size_t)w * h * 2); dnw = w; dnh = h; }
                int exp_lines, gain; eo_frame_ae(&exp_lines, &gain);
                int q5 = dn && tdn_process(buf, stride, w, h, dn, exp_lines, gain);
                int cx, cy, cw, ch;
                zoom_crop_43(w, h, mjpeg_zoom(), dw, dh, &cx, &cy, &cw, &ch);
                isp_scale_tonemap(q5 ? (const uint8_t *)dn : buf, q5 ? w * 2 : stride,
                                  cx, cy, cw, ch, disp, dw, dh, q5);
                mjpeg_publish(disp, dw, dh);
            }
        } else {
            usleep(2000);                      /* poll; sensor emits at the operating fps */
        }
    }

    fprintf(stderr, "eo_pipeline: shutting down\n");
    eo_stop();
    free(disp);
    free(dn);
    return 0;
}
