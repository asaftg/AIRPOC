/* main.c - trackerd: the EO tracker daemon (:8095).
 *
 * Consumes the detector's SSE /stream (:8094), turns per-frame boxes into
 * persistent smoothed/coasted tracks with stable IDs, and serves them on
 * /stream + /stats + /ctl plus the airpoc.trk_wire recorder tap. When the
 * operator engages a target (/ctl?engage=<tid>) it runs a 60 fps NCC lock on
 * the raw EO frames for guidance-grade, camera-rate az/el.
 *
 * Threads: det_feed (SSE consumer -> on_det_frame), lock (60 fps), heartbeat
 * (1 s). All three publish through publish_wire() under g_lk. See README.
 */
#define _GNU_SOURCE
#include "core.h"
#include "config.h"
#include "http.h"
#include "emit.h"
#include "det_feed.h"
#include "eo_reader.h"
#include "ego.h"
#include "lock.h"
#include "airpoc_tap.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

static struct {
    pthread_mutex_t lk;
    TrkCore   *core;
    double     ifov_rad;
    /* knobs mirror */
    int        engage;
    int        lock_enabled;
    /* timing / last header */
    uint64_t   last_det_pub_ns;   /* t_pub of previous det frame (dt from this) */
    uint64_t   last_frame_id;
    uint64_t   last_t_src_ns;
    uint64_t   last_t_pub_ns;
    /* health */
    unsigned long errors;
    int        degraded;
    /* wire buffer */
    char       json[128 * 1024];
    AirTap     tap;
    int        tap_ok;
    /* fps meters */
    double     det_fps, out_fps;
    uint64_t   last_out_ns;
    int        no_eo;             /* --no-eo: skip lock/ego (bench without a camera) */
} S;

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Build + publish one wire frame from the given emitted tracks. Caller holds g_lk. */
static void publish_wire(const TrkOut *tracks, int n, int connected, uint64_t t_src_ns)
{
    TrkHdr h = {
        .frame_id = S.last_frame_id,
        .t_src_ns = t_src_ns,
        .t_pub_ns = S.last_t_pub_ns,
        .t_out_ns = now_ns(),
        .connected = connected,
        .mode = (S.engage >= 0) ? "track" : "stare",
        .engaged = S.engage,
        .img_w = EO_IMG_W, .img_h = EO_IMG_H,
        .ifov_rad = S.ifov_rad,
    };
    size_t len = trk_frame_json(S.json, sizeof S.json, &h, tracks, n);
    http_publish(S.json, len);
    if (S.tap_ok) {
        uint32_t meta[6] = { (uint32_t)S.last_frame_id, (uint32_t)n, 0, 0, 0, 0 };
        tap_write(&S.tap, S.json, (uint32_t)len, t_src_ns, meta);
    }
    /* out fps meter */
    uint64_t t = now_ns();
    if (S.last_out_ns) {
        double dt = (t - S.last_out_ns) / 1e9;
        if (dt > 1e-4) S.out_fps += 0.1 * (1.0 / dt - S.out_fps);
    }
    S.last_out_ns = t;
}

/* ---- detector frame callback (feed thread) ---- */
static EoReader *g_ego_eo;
static Ego      *g_ego;
static uint16_t  g_ego_frame[EO_IMG_W * EO_IMG_H];

static void on_det_frame(const TrkDet *dets, int n, const DetMeta *m, void *user)
{
    (void)user;
    pthread_mutex_lock(&S.lk);

    if (m->have_ifov && m->ifov_rad > 0) S.ifov_rad = m->ifov_rad;

    /* dt from t_pub (CLOCK_MONOTONIC); t_src is only a correlation key. */
    double dt = 1.0 / TRK_GATE_REF_FPS;
    if (S.last_det_pub_ns && m->t_pub_ns > S.last_det_pub_ns)
        dt = (m->t_pub_ns - S.last_det_pub_ns) / 1e9;
    S.last_det_pub_ns = m->t_pub_ns;
    if (dt > 1e-4) S.det_fps += 0.1 * (1.0 / dt - S.det_fps);
    S.last_frame_id = m->frame_id;
    S.last_t_src_ns = m->t_src_ns;
    S.last_t_pub_ns = m->t_pub_ns;

    /* ego shift since last tick (0 without a camera / static mount) */
    double ex = 0, ey = 0;
    if (!S.no_eo && g_ego_eo) {
        int w, h; uint64_t seq, ts; uint32_t meta[6];
        if (eo_latest(g_ego_eo, g_ego_frame, sizeof g_ego_frame, &w, &h, &seq, &ts, meta))
            ego_update(g_ego, g_ego_frame, w, h, &ex, &ey);
    }

    TrkOut out[TRK_MAX_TRACKS];
    int no = trk_core_step(S.core, dets, n, m->t_src_ns, dt, ex, ey, S.engage,
                           out, TRK_MAX_TRACKS);
    publish_wire(out, no, 1, m->t_src_ns);

    int live, emitted; trk_core_counts(S.core, &live, &emitted);
    pthread_mutex_unlock(&S.lk);

    http_set_tracks(live, emitted);
    http_set_feed(1, eo_connected(g_ego_eo), S.det_fps, S.out_fps);
}

/* ---- /ctl callback ---- */
static void on_ctl(const TrkCtl *c, void *user)
{
    (void)user;
    pthread_mutex_lock(&S.lk);
    int was = S.engage;
    S.engage = c->engage;
    S.lock_enabled = c->lock;
    TrkKnobs k = { .gate_base = c->gate_base, .confirm = c->confirm,
                   .coast_s = c->coast_s, .clutter_s = c->clutter_s };
    trk_core_set_knobs(S.core, &k);
    pthread_mutex_unlock(&S.lk);
    /* reflect the selection in /stats at once; the lock thread updates on/score when
     * it actually locks (stays off without a camera or below the score floor). */
    http_set_lock(c->engage, 0, 0);
    if (was != c->engage) fprintf(stderr, "trackerd: engage %d -> %d\n", was, c->engage);
}

/* ---- 60 fps lock thread ---- */
static void *lock_thread(void *arg)
{
    (void)arg;
    if (S.no_eo) return NULL;
    EoReader *eo = eo_open(EO_TAP_NAME);
    Lock *lk = lock_new();
    static uint16_t frame[EO_IMG_W * EO_IMG_H];
    int cur_engaged = -1, meta_hold = 0;
    uint32_t prev_illum = 0, prev_exp = 0, prev_gain = 0;

    for (;;) {
        pthread_mutex_lock(&S.lk);
        int engage = S.engage, en = S.lock_enabled;
        pthread_mutex_unlock(&S.lk);

        if (engage < 0 || !en) {
            if (cur_engaged != -1) { lock_reset(lk); cur_engaged = -1; }
            usleep(10000);
            continue;
        }
        if (engage != cur_engaged) { lock_reset(lk); cur_engaged = engage; }

        int w, h; uint64_t seq, ts; uint32_t meta[6];
        if (!eo_latest(eo, frame, sizeof frame, &w, &h, &seq, &ts, meta)) { usleep(2000); continue; }

        /* freeze template refresh across an AE / illuminator step */
        uint32_t illum = meta[EO_META_ILLUM], exp = meta[EO_META_EXPLINES], gain = meta[EO_META_GAIN];
        int step = (illum != prev_illum) || (exp != prev_exp) || (gain != prev_gain);
        prev_illum = illum; prev_exp = exp; prev_gain = gain;
        if (step) meta_hold = TRK_LOCK_META_HOLD;

        pthread_mutex_lock(&S.lk);
        double cx, cy, bw, bh; int cls;
        int have = trk_core_engaged_box(S.core, engage, &cx, &cy, &bw, &bh, &cls);
        pthread_mutex_unlock(&S.lk);
        if (!have) { usleep(2000); continue; }

        /* seed template from the NN box if we have none (or after a reset) and not
         * in a metadata-step freeze */
        if (!lock_has_template(lk) && meta_hold == 0)
            lock_set_template(lk, frame, w, h, cx, cy, bw, bh);

        double ox = cx, oy = cy, score = 0;
        if (lock_has_template(lk))
            lock_track(lk, frame, w, h, cx, cy, &ox, &oy, &score);

        if (score >= TRK_LOCK_SCORE_MIN) {
            /* good match: nudge the engaged track and, if not frozen, refresh the
             * template from the detector-anchored box (drift control) */
            pthread_mutex_lock(&S.lk);
            trk_core_lock_update(S.core, engage, ox, oy, score, ts);
            TrkOut out[TRK_MAX_TRACKS];
            int no = trk_core_snapshot(S.core, engage, out, TRK_MAX_TRACKS);
            publish_wire(out, no, det_feed_connected(1000000000ull), ts);
            pthread_mutex_unlock(&S.lk);
            http_set_lock(engage, 1, score);
        } else {
            http_set_lock(engage, 0, score);   /* low score = HOLD, not loss */
        }
        if (meta_hold > 0) meta_hold--;
        /* pace to ~camera rate; eo_latest already drains to newest so we never lag */
        usleep(3000);
    }
    lock_free(lk); eo_close(eo);
    return NULL;
}

/* ---- 1 s heartbeat: emit even when the detector is silent ---- */
static void *heartbeat_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(1000000);
        int connected = det_feed_connected(1500000000ull);
        if (connected) continue;   /* det ticks are driving the wire */
        pthread_mutex_lock(&S.lk);
        /* advance the tracker one virtual tick so confirmed tracks coast then die */
        TrkOut out[TRK_MAX_TRACKS];
        int no = trk_core_step(S.core, NULL, 0, S.last_t_src_ns, 1.0, 0, 0, S.engage,
                               out, TRK_MAX_TRACKS);
        publish_wire(out, no, 0, S.last_t_src_ns);
        int live, emitted; trk_core_counts(S.core, &live, &emitted);
        pthread_mutex_unlock(&S.lk);
        http_set_tracks(live, emitted);
        http_set_feed(0, S.no_eo ? 0 : eo_connected(g_ego_eo), 0, S.out_fps);
    }
    return NULL;
}

static void usage(const char *p)
{
    fprintf(stderr,
      "usage: %s [-p port] [-d host:port] [-i ifov_urad | -f focal_mm -x pixel_um]\n"
      "          [-t eo_tap] [--no-eo]\n", p);
}

int main(int argc, char **argv)
{
    int port = TRK_PORT_DEFAULT;
    char det_host[64] = DET_STREAM_HOST; int det_port = DET_STREAM_PORT;
    double ifov = EO_IFOV_RAD_DEFAULT;
    double focal = EO_FOCAL_MM_DEFAULT, pixel = EO_PIXEL_UM_DEFAULT;
    int have_ifov = 0;
    char eo_tap[64] = EO_TAP_NAME;

    static struct option lo[] = {
        {"no-eo", no_argument, 0, 1000}, {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:d:i:f:x:t:h", lo, NULL)) != -1) {
        switch (c) {
        case 'p': port = atoi(optarg); break;
        case 'd': { char *csep = strchr(optarg, ':');
                    if (csep) { *csep = 0; snprintf(det_host, sizeof det_host, "%s", optarg);
                                det_port = atoi(csep + 1); }
                    else snprintf(det_host, sizeof det_host, "%s", optarg); } break;
        case 'i': ifov = atof(optarg) * 1e-6; have_ifov = 1; break;
        case 'f': focal = atof(optarg); break;
        case 'x': pixel = atof(optarg); break;
        case 't': snprintf(eo_tap, sizeof eo_tap, "%s", optarg); break;
        case 1000: S.no_eo = 1; break;
        case 'h': default: usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }
    (void)eo_tap;
    if (!have_ifov) ifov = pixel * 1e-6 / (focal * 1e-3);

    pthread_mutex_init(&S.lk, NULL);
    S.core = trk_core_new();
    if (!S.core) { fprintf(stderr, "trackerd: core alloc failed\n"); return 1; }
    S.ifov_rad = ifov;
    S.engage = -1;
    S.lock_enabled = 1;
    S.det_fps = S.out_fps = 0;

    /* recorder tap (best-effort: a create failure just disables recording) */
    if (tap_create(&S.tap, TRK_TAP_NAME, TRK_TAP_SLOTS, TRK_TAP_BYTES,
                   "{\"name\":\"trk_wire\",\"fmt\":\"json\"}") == 0)
        S.tap_ok = 1;
    else
        fprintf(stderr, "trackerd: trk_wire tap unavailable, recording disabled\n");

    http_set_info(TRK_VERSION, ifov * 1e6, EO_IMG_W, EO_IMG_H);
    http_set_ctl_cb(on_ctl, NULL);
    if (http_start(port) != 0) { fprintf(stderr, "trackerd: http_start failed\n"); return 1; }

    if (!S.no_eo) { g_ego_eo = eo_open(eo_tap); g_ego = ego_new(); }

    if (det_feed_start(det_host, det_port, on_det_frame, NULL) != 0) {
        fprintf(stderr, "trackerd: det_feed_start failed\n"); return 1;
    }

    pthread_t th_lock, th_hb;
    pthread_create(&th_lock, NULL, lock_thread, NULL);
    pthread_create(&th_hb, NULL, heartbeat_thread, NULL);

    fprintf(stderr, "trackerd %s on :%d, detector %s:%d, ifov %.1f urad/px%s\n",
            TRK_VERSION, port, det_host, det_port, ifov * 1e6, S.no_eo ? " (no-eo)" : "");

    pthread_join(th_hb, NULL);   /* runs forever */
    return 0;
}
