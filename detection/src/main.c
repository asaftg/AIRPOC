/* detectiond — AIRPOC EO object detector daemon.
 *
 *   ./detectiond [-p 8094] [-t airpoc.eo_y10] [-e engine] [-s sidecar]
 *                [-f focal_mm] [-x pixel_um] [-i ifov_urad] [-E]
 *
 * Reads the EO camera tap and finds people / vehicles / drones, publishing
 * per-frame boxes on :8094 (/stream + /stats + /ctl), same contract shape as the
 * radar daemon, plus the airpoc.det_wire recorder tap.
 *
 * Two detection paths:
 *   - Appearance model (TensorRT, -e engine): runs on every cadence-th frame in
 *     the main loop; emits classified boxes (src "app").
 *   - Motion worker (motion.c): runs in its own thread at camera rate with its own
 *     tap reader (the tap allows many readers), catching any moving target the
 *     model missed; emits unclassified movers (src "mot"). -E = ECC stabilizer
 *     (default identity, correct for a static mount).
 * Where a mover overlaps a model box, the model box wins -> one box per target.
 * With no engine the model path is a heartbeat (empty dets); the motion path and
 * the whole contract still run, so the GUI/recorder integrate before the model.
 */
#define _GNU_SOURCE
#include "config.h"
#include "source.h"
#include "http.h"
#include "emit.h"
#include "infer.h"
#include "motion.h"
#include "coco.h"
#include "airpoc_tap.h"
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DET_WIRE_NAME   "airpoc.det_wire"
#define DET_WIRE_SLOTS  16
#define DET_WIRE_BYTES  (128 * 1024)
#define JSON_CAP        (128 * 1024)
#define MAX_DETS_CAP    512
#define MAX_MOV_CAP     128

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* Movers produced by the motion thread, read by the main (emit) loop. */
static pthread_mutex_t g_mov_lock = PTHREAD_MUTEX_INITIALIZER;
static Mover  g_movers[MAX_MOV_CAP];
static int    g_nmov = 0;
static int    g_stab_fail = 0;
static double g_motion_fps = 0;

/* Shared config for the motion thread. */
typedef struct {
    const char *tap_name;
    int use_ecc;
} MotionArgs;

static void on_ctl(const DetKnobs *k, void *user)
{
    (void)user;
    fprintf(stderr, "detectiond: /ctl conf=%.2f cadence=%d motion=%d max_dets=%d "
            "nms=%.2f mot_k=%.1f mot_window_s=%.1f mot_persist=%d mot_down=%d mot_fps=%d\n",
            k->conf, k->cadence, k->motion, k->max_dets, k->nms, k->mot_k,
            k->mot_window_s, k->mot_persist, k->mot_down, k->mot_fps);
}

static void *motion_thread(void *arg)
{
    MotionArgs *ma = arg;
    FrameSource *src = tap_source_open(ma->tap_name);
    if (!src) { fprintf(stderr, "detectiond: motion thread init failed\n"); return NULL; }

    /* The worker is built lazily and rebuilt when mot_down changes (the downscale is
     * fixed at construction). Motion runs at mot_fps (not the full camera rate): far/
     * small movers are slow in pixels, so a lower rate is what makes native affordable;
     * we process every step-th captured frame and pass the effective rate on. */
    MotionWorker *mw = NULL;
    int cur_down = -1;
    Mover local[MAX_MOV_CAP];
    uint64_t last_t = 0, last_proc_t = 0;
    double fps = 0, proc_fps = 0;
    long fno = 0;
    DetFrame f;
    while (g_run) {
        if (src->next(src, &f) != 1) { usleep(2000); continue; }
        if (last_t && f.t_src_ns > last_t) {
            double inst = 1e9 / (double)(f.t_src_ns - last_t);
            fps = fps > 0 ? 0.9 * fps + 0.1 * inst : inst;
        }
        last_t = f.t_src_ns;
        fno++;

        DetKnobs k; http_get_knobs(&k);
        if (!k.motion) {
            pthread_mutex_lock(&g_mov_lock);
            g_nmov = 0; g_stab_fail = 0; g_motion_fps = 0;
            pthread_mutex_unlock(&g_mov_lock);
            continue;
        }

        if (k.mot_down != cur_down) {          /* (re)build worker on downscale change */
            if (mw) motion_free(mw);
            cur_down = k.mot_down > 0 ? k.mot_down : 1;
            mw = motion_new(EO_IMG_W, EO_IMG_H, cur_down, ma->use_ecc);
            if (!mw) { fprintf(stderr, "detectiond: motion_new(down=%d) failed\n", cur_down); usleep(100000); continue; }
        }

        double cap_fps = fps > 1.0 ? fps : 60.0;
        int step = (int)lround(cap_fps / (k.mot_fps > 0 ? k.mot_fps : 15));
        if (step < 1) step = 1;
        if (fno % step != 0) continue;         /* rate-limit to ~mot_fps */

        int nmov = 0, sf = 0;
        MotionParams mp = { .k_mad = k.mot_k, .window_s = k.mot_window_s,
                            .fps = cap_fps / step, .persist = k.mot_persist };
        nmov = motion_process(mw, f.y10, f.w, f.h, &mp, local, MAX_MOV_CAP, &sf);

        uint64_t now = tap_now_ns();
        if (last_proc_t) {
            double inst = 1e9 / (double)(now - last_proc_t);
            proc_fps = proc_fps > 0 ? 0.9 * proc_fps + 0.1 * inst : inst;
        }
        last_proc_t = now;

        pthread_mutex_lock(&g_mov_lock);
        memcpy(g_movers, local, (size_t)nmov * sizeof(Mover));
        g_nmov = nmov; g_stab_fail = sf; g_motion_fps = proc_fps;
        pthread_mutex_unlock(&g_mov_lock);
    }
    src->close(src);
    if (mw) motion_free(mw);
    return NULL;
}

/* True if a mover overlaps a model box (IoU>0.3 or its centre is inside). */
static int overlaps(const Mover *m, const DetBox *dets, int nd)
{
    float mx1 = m->cx - m->w / 2, my1 = m->cy - m->h / 2;
    float mx2 = m->cx + m->w / 2, my2 = m->cy + m->h / 2;
    for (int i = 0; i < nd; i++) {
        const DetBox *d = &dets[i];
        float dx1 = d->cx - d->w / 2, dy1 = d->cy - d->h / 2;
        float dx2 = d->cx + d->w / 2, dy2 = d->cy + d->h / 2;
        if (m->cx >= dx1 && m->cx <= dx2 && m->cy >= dy1 && m->cy <= dy2) return 1;
        float ix1 = mx1 > dx1 ? mx1 : dx1, iy1 = my1 > dy1 ? my1 : dy1;
        float ix2 = mx2 < dx2 ? mx2 : dx2, iy2 = my2 < dy2 ? my2 : dy2;
        float iw = ix2 - ix1, ih = iy2 - iy1;
        if (iw > 0 && ih > 0) {
            float inter = iw * ih;
            float ua = m->w * m->h + d->w * d->h - inter;
            if (ua > 0 && inter / ua > 0.3f) return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int port = 8094;
    const char *tap_name = EO_TAP_NAME;
    const char *engine_path = NULL, *sidecar_path = NULL;
    double focal_mm = EO_FOCAL_MM_DEFAULT, pixel_um = EO_PIXEL_UM_DEFAULT;
    double ifov_override_urad = 0;
    int use_ecc = 0, opt;

    while ((opt = getopt(argc, argv, "p:t:e:s:f:x:i:Eh")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 't': tap_name = optarg; break;
            case 'e': engine_path = optarg; break;
            case 's': sidecar_path = optarg; break;
            case 'f': focal_mm = atof(optarg); break;
            case 'x': pixel_um = atof(optarg); break;
            case 'i': ifov_override_urad = atof(optarg); break;
            case 'E': use_ecc = 1; break;
            case 'h':
            default:
                fprintf(stderr, "usage: %s [-p port] [-t tap] [-e engine] [-s sidecar] "
                        "[-f focal_mm] [-x pixel_um] [-i ifov_urad] [-E]\n", argv[0]);
                return 2;
        }
    }

    double ifov_rad = ifov_override_urad > 0 ? ifov_override_urad * 1e-6
                                             : (pixel_um * 1e-6) / (focal_mm * 1e-3);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (http_start(port) != 0) { fprintf(stderr, "detectiond: http_start failed\n"); return 1; }
    http_set_info(DET_VERSION, ifov_rad * 1e6, EO_IMG_W, EO_IMG_H);
    http_set_ctl_cb(on_ctl, NULL);

    /* Optional appearance model. Absent -> heartbeat mode (empty dets). */
    InferEngine *engine = NULL;
    if (engine_path) {
        char err[128] = {0};
        engine = infer_open(engine_path, sidecar_path, err, sizeof err);
        if (!engine) fprintf(stderr, "detectiond: engine '%s' not loaded: %s (running model-less)\n", engine_path, err);
        else fprintf(stderr, "detectiond: model %s (%s), %d classes\n",
                     infer_model_name(engine), infer_precision(engine), infer_num_classes(engine));
    }
    const char *model_name = engine ? infer_model_name(engine) : "none";

    fprintf(stderr, "detectiond %s: http://0.0.0.0:%d/  tap=%s  ifov=%.1f urad/px  model=%s\n",
            DET_VERSION, port, tap_name, ifov_rad * 1e6, model_name);

    AirTap wire_tap;
    if (tap_create(&wire_tap, DET_WIRE_NAME, DET_WIRE_SLOTS, DET_WIRE_BYTES,
                   "{\"name\":\"det_wire\"}") < 0)
        fprintf(stderr, "detectiond: det_wire tap unavailable — recording disabled for it\n");

    MotionArgs ma = { tap_name, use_ecc };
    pthread_t mt;
    pthread_create(&mt, NULL, motion_thread, &ma);

    FrameSource *src = tap_source_open(tap_name);
    char *json = malloc(JSON_CAP);
    InferBox *iboxes = malloc(sizeof(InferBox) * MAX_DETS_CAP);
    DetBox *dets = malloc(sizeof(DetBox) * MAX_DETS_CAP);
    DetBox *movers = malloc(sizeof(DetBox) * MAX_MOV_CAP);
    Mover *msnap = malloc(sizeof(Mover) * MAX_MOV_CAP);
    if (!src || !json || !iboxes || !dets || !movers || !msnap) { perror("alloc"); return 1; }

    uint64_t last_t_src = 0, last_rx_ns = 0;
    double fps = 0, det_fps = 0, last_infer_ms = 0, infer_ms_max = 0;
    unsigned long gaps = 0;
    uint64_t last_tick_t = 0;
    DetFrame f;

    while (g_run) {
        if (src->next(src, &f) != 1) {
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
        http_set_tap(1, fps, gaps, drops, v4l2_seq);

        DetKnobs k; http_get_knobs(&k);
        int cadence = k.cadence > 0 ? k.cadence : 1;
        if ((v4l2_seq % (uint32_t)cadence) != 0) continue;   /* not a detector tick */

        /* Appearance model on this tick. */
        int nd = 0;
        if (engine) {
            int nb = infer_run(engine, f.y10, f.w, f.h, (float)k.conf, (float)k.nms, k.max_dets,
                               iboxes, MAX_DETS_CAP, &last_infer_ms);
            if (last_infer_ms > infer_ms_max) infer_ms_max = last_infer_ms;
            for (int i = 0; i < nb && nd < MAX_DETS_CAP; i++) {
                const char *cls = coco_to_airpoc(iboxes[i].cls);
                if (!cls) continue;                          /* not one of our classes */
                dets[nd].src = "app"; dets[nd].cls = cls; dets[nd].conf = iboxes[i].conf;
                dets[nd].age = -1;
                dets[nd].cx = iboxes[i].cx; dets[nd].cy = iboxes[i].cy;
                dets[nd].w = iboxes[i].w; dets[nd].h = iboxes[i].h;
                nd++;
            }
        }

        /* Latest movers snapshot; drop any that overlap a model box. */
        pthread_mutex_lock(&g_mov_lock);
        int nmsnap = g_nmov; int sf = g_stab_fail; double mfps = g_motion_fps;
        memcpy(msnap, g_movers, (size_t)nmsnap * sizeof(Mover));
        pthread_mutex_unlock(&g_mov_lock);

        int nm = 0;
        for (int i = 0; i < nmsnap && nm < MAX_MOV_CAP; i++) {
            if (overlaps(&msnap[i], dets, nd)) continue;
            movers[nm].src = "mot"; movers[nm].cls = 0; movers[nm].conf = msnap[i].conf;
            movers[nm].age = msnap[i].age;
            movers[nm].cx = msnap[i].cx; movers[nm].cy = msnap[i].cy;
            movers[nm].w = msnap[i].w; movers[nm].h = msnap[i].h;
            nm++;
        }

        /* Detector-tick rate. */
        uint64_t now = tap_now_ns();
        if (last_tick_t) {
            double inst = 1e9 / (double)(now - last_tick_t);
            det_fps = det_fps > 0 ? 0.8 * det_fps + 0.2 * inst : inst;
        }
        last_tick_t = now;

        DetHdr h = {
            .frame_id = v4l2_seq, .t_src_ns = f.t_src_ns, .t_pub_ns = f.t_pub_ns,
            .t_out_ns = now, .night = (int)EO_ILLUM_ON(illum),
            .illum_on = EO_ILLUM_ON(illum), .illum_present = EO_ILLUM_PRESENT(illum),
            .illum_power = EO_ILLUM_POWER(illum), .illum_fov10 = EO_ILLUM_FOV10(illum),
            .img_w = f.w, .img_h = f.h, .model = model_name, .ifov_rad = ifov_rad,
            .tap_gaps = gaps, .drops_cum = drops,
        };
        size_t len = det_frame_json(json, JSON_CAP, &h, dets, nd, movers, nm);
        if (len >= JSON_CAP) len = JSON_CAP - 1;

        http_publish(json, len);
        uint32_t wmeta[TAP_META_WORDS] = { v4l2_seq, (uint32_t)nd, (uint32_t)nm, 0, 0, 0 };
        tap_write(&wire_tap, json, (uint32_t)len, tap_now_ns(), wmeta);

        if (engine)
            http_set_det(det_fps, last_infer_ms, infer_ms_max, last_infer_ms, infer_ms_max,
                         model_name, infer_precision(engine));
        http_set_motion(mfps, sf ? 100.0 : 0.0, nmsnap);
    }

    fprintf(stderr, "detectiond: shutting down\n");
    g_run = 0;
    pthread_join(mt, NULL);
    tap_destroy(&wire_tap);
    if (engine) infer_close(engine);
    src->close(src);
    free(json); free(iboxes); free(dets); free(movers); free(msnap);
    return 0;
}
