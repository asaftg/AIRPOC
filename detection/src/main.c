/* detectiond — AIRPOC EO object detector daemon.
 *
 *   ./detectiond [-p 8094] [-t airpoc.eo_y10] [-f focal_mm] [-x pixel_um] [-i ifov_urad]
 *
 * Phase 1: consume the EO camera tap (airpoc.eo_y10), track feed health, and
 * serve /stream (SSE), /stats, /ctl on :8094 — the same contract shape as the
 * radar daemon. No GPU yet: the detection model and motion worker land in later
 * phases behind this scaffold. Each detection message is emitted with empty
 * dets[]/movers[] and model "none" (a heartbeat) so downstream wiring, the
 * launcher health check, and the recorder tap can all be proven now.
 *
 * The camera pipeline is never touched: we are a separate reader of its shm
 * ring, and we self-heal across EO restarts. Best-effort airpoc.det_wire tap
 * mirrors the /stream payload for the recorder (no-op if it can't be created).
 */
#define _GNU_SOURCE
#include "config.h"
#include "source.h"
#include "http.h"
#include "emit.h"
#include "airpoc_tap.h"
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DET_WIRE_NAME   "airpoc.det_wire"
#define DET_WIRE_SLOTS  16
#define DET_WIRE_BYTES  (128 * 1024)
#define JSON_CAP        (128 * 1024)

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* /ctl handler — knob state lives in http.c; the daemon reads it live with
 * http_get_knobs(). We only log applied changes here. */
static void on_ctl(const DetKnobs *k, void *user)
{
    (void)user;
    fprintf(stderr, "detectiond: /ctl conf=%.2f cadence=%d motion=%d max_dets=%d "
            "mot_k=%.1f mot_persist=%d\n",
            k->conf, k->cadence, k->motion, k->max_dets, k->mot_k, k->mot_persist);
}

int main(int argc, char **argv)
{
    int    port = 8094;
    const char *tap_name = EO_TAP_NAME;
    double focal_mm = EO_FOCAL_MM_DEFAULT, pixel_um = EO_PIXEL_UM_DEFAULT;
    double ifov_override_urad = 0;   /* >0 wins over focal/pixel */
    int opt;

    while ((opt = getopt(argc, argv, "p:t:f:x:i:h")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 't': tap_name = optarg; break;
            case 'f': focal_mm = atof(optarg); break;
            case 'x': pixel_um = atof(optarg); break;
            case 'i': ifov_override_urad = atof(optarg); break;
            case 'h':
            default:
                fprintf(stderr, "usage: %s [-p port] [-t tap] [-f focal_mm] "
                        "[-x pixel_um] [-i ifov_urad]\n", argv[0]);
                return 2;
        }
    }

    /* IFOV (rad/px): direct override, else pixel/focal. NOTE: the lens was
     * changed on the bench — confirm the installed focal length and pass -f/-i. */
    double ifov_rad = ifov_override_urad > 0 ? ifov_override_urad * 1e-6
                                             : (pixel_um * 1e-6) / (focal_mm * 1e-3);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);        /* dropped SSE clients must not kill us */

    /* HTTP first so the launcher's port-bound health check passes even before a
     * camera feed exists. */
    if (http_start(port) != 0) { fprintf(stderr, "detectiond: http_start failed\n"); return 1; }
    http_set_info(DET_VERSION, ifov_rad * 1e6, EO_IMG_W, EO_IMG_H);
    http_set_ctl_cb(on_ctl, NULL);
    fprintf(stderr, "detectiond %s: http://0.0.0.0:%d/  tap=%s  ifov=%.1f urad/px\n",
            DET_VERSION, port, tap_name, ifov_rad * 1e6);

    /* Recorder tap: byte-verbatim /stream JSON. Best-effort — no-op on failure. */
    AirTap wire_tap;
    if (tap_create(&wire_tap, DET_WIRE_NAME, DET_WIRE_SLOTS, DET_WIRE_BYTES,
                   "{\"name\":\"det_wire\"}") < 0)
        fprintf(stderr, "detectiond: det_wire tap unavailable — recording disabled for it\n");

    FrameSource *src = tap_source_open(tap_name);
    if (!src) { fprintf(stderr, "detectiond: out of memory\n"); return 1; }

    char *json = malloc(JSON_CAP);
    if (!json) { perror("malloc"); return 1; }

    uint64_t last_t_src = 0;
    double   fps = 0;
    unsigned long gaps = 0;
    uint64_t last_rx_ns = 0;
    DetFrame f;

    while (g_run) {
        int r = src->next(src, &f);
        if (r != 1) {
            /* No frame this poll. Mark disconnected if the feed has gone quiet. */
            if (!src->connected(src) || (last_rx_ns && tap_now_ns() - last_rx_ns > 1000000000ull))
                http_set_tap(0, 0, gaps, 0, 0);
            usleep(2000);
            continue;
        }
        last_rx_ns = tap_now_ns();
        gaps += (unsigned long)f.gap_before;

        if (last_t_src && f.t_src_ns > last_t_src) {
            double inst = 1e9 / (double)(f.t_src_ns - last_t_src);
            fps = fps > 0 ? 0.9 * fps + 0.1 * inst : inst;
        }
        last_t_src = f.t_src_ns;

        uint32_t v4l2_seq = f.meta[EO_META_V4L2SEQ];
        uint32_t illum    = f.meta[EO_META_ILLUM];
        unsigned long drops = f.meta[EO_META_DROPS];

        /* Health every frame so /stats fps is live between ticks. */
        http_set_tap(1, fps, gaps, drops, v4l2_seq);

        /* Emit a detection message every cadence-th captured frame — the same
         * gate the GPU detector will use. In phase 1 the arrays are empty. */
        DetKnobs k; http_get_knobs(&k);
        int cadence = k.cadence > 0 ? k.cadence : 1;
        if ((v4l2_seq % (uint32_t)cadence) != 0) continue;

        DetHdr h = {
            .frame_id = v4l2_seq,
            .t_src_ns = f.t_src_ns,
            .t_pub_ns = f.t_pub_ns,
            .t_out_ns = tap_now_ns(),
            .night = (int)EO_ILLUM_ON(illum),
            .illum_on = EO_ILLUM_ON(illum),
            .illum_present = EO_ILLUM_PRESENT(illum),
            .illum_power = EO_ILLUM_POWER(illum),
            .illum_fov10 = EO_ILLUM_FOV10(illum),
            .img_w = f.w, .img_h = f.h,
            .model = "none",
            .ifov_rad = ifov_rad,
            .tap_gaps = gaps,
            .drops_cum = drops,
        };
        size_t len = det_frame_json(json, JSON_CAP, &h, NULL, 0, NULL, 0);
        if (len >= JSON_CAP) len = JSON_CAP - 1;

        http_publish(json, len);
        uint32_t wmeta[TAP_META_WORDS] = { v4l2_seq, 0, 0, 0, 0, 0 };
        tap_write(&wire_tap, json, (uint32_t)len, tap_now_ns(), wmeta);
    }

    fprintf(stderr, "detectiond: shutting down\n");
    tap_destroy(&wire_tap);
    src->close(src);
    free(json);
    return 0;
}
