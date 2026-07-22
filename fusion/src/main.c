/* main.c - fusiond: the late-fusion daemon (:8096).
 *
 * Consumes the radar tracker SSE (:8092) and the EO tracker SSE (:8095),
 * joins the two track streams into one target picture (fusion-assigned
 * global ids; radar range/doppler + EO angles + EO class on fused rows),
 * and serves it on /stream + /stats + /ctl plus the airpoc.fus_wire
 * recorder tap. Never in the radar->gimbal EO-blind critical path: that
 * chain consumes :8092 directly and runs with this daemon dead.
 *
 * Threads: two SSE feed clients (-> on_rad_frame / on_trk_frame), http
 * server, 1 Hz heartbeat. All publishes go through publish_wire() under
 * S.lk. See README.
 */
#define _GNU_SOURCE
#include "core.h"
#include "config.h"
#include "http.h"
#include "emit.h"
#include "trim.h"
#include "sse_client.h"
#include "rad_parse.h"
#include "trk_parse.h"
#include "airpoc_tap.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

#define D2R (M_PI / 180.0)

static struct {
    pthread_mutex_t lk;
    FusCore   *core;
    SseClient *rad, *trk;
    /* last upstream headers */
    uint64_t   rad_frame_id, eo_frame_id;
    int        eo_engaged;
    /* wire */
    uint64_t   out_seq;
    char       json[128 * 1024];
    AirTap     tap;
    int        tap_ok;
    /* health / meters */
    unsigned long errors;
    double     rad_fps, trk_fps, out_fps;
    uint64_t   last_rad_ns, last_trk_ns, last_out_ns;
} S;

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Build + publish one wire frame. Caller holds S.lk. */
static void publish_wire(const FusOut *rows, int n)
{
    FusKnobs k; fus_core_get_knobs(S.core, &k);
    double ea, ee; int en = fus_core_trim_est(S.core, &ea, &ee);
    FusHdr h = {
        .rad_connected = S.rad ? sse_client_connected(S.rad, 1000000000ull) : 0,
        .trk_connected = S.trk ? sse_client_connected(S.trk, 2000000000ull) : 0,
        .frame_id = ++S.out_seq,
        .rad_frame_id = S.rad_frame_id, .eo_frame_id = S.eo_frame_id,
        .eo_engaged = S.eo_engaged,
        .t_out_ns = now_ns(),
        .trim_az_deg = k.trim_az * 180.0 / M_PI,
        .trim_el_deg = k.trim_el * 180.0 / M_PI,
        .est_az_deg = ea * 180.0 / M_PI, .est_el_deg = ee * 180.0 / M_PI,
        .est_n = en,
    };
    size_t len = fus_frame_json(S.json, sizeof S.json, &h, rows, n);
    http_publish(S.json, len);
    if (S.tap_ok) {
        int f, e, r; fus_core_counts(S.core, &f, &e, &r);
        uint32_t meta[6] = { (uint32_t)S.out_seq, (uint32_t)f, (uint32_t)e, (uint32_t)r, 0, 0 };
        tap_write(&S.tap, S.json, (uint32_t)len, h.t_out_ns, meta);
    }
    uint64_t t = now_ns();
    if (S.last_out_ns) {
        double dt = (double)(t - S.last_out_ns) / 1e9;
        if (dt > 1e-4) S.out_fps += 0.1 * (1.0 / dt - S.out_fps);
    }
    S.last_out_ns = t;
}

static void push_stats(void)
{
    int f, e, r; fus_core_counts(S.core, &f, &e, &r);
    http_set_tracks(f, e, r);
    http_set_feeds(S.rad ? sse_client_connected(S.rad, 1000000000ull) : 0,
                   S.trk ? sse_client_connected(S.trk, 2000000000ull) : 0,
                   S.rad_fps, S.trk_fps, S.out_fps);
    double ea, ee; int n = fus_core_trim_est(S.core, &ea, &ee);
    http_set_trim_info(NULL, ea / D2R, ee / D2R, n);
    http_set_degraded(0, S.errors, "");
}

/* ---- feed callbacks (their SSE threads) ---- */

static void on_rad_frame(const char *json, void *user)
{
    (void)user;
    FusRadTgt tgts[FUS_MAX_RAD];
    RadMeta m;
    int n = rad_parse(json, tgts, FUS_MAX_RAD, &m);
    pthread_mutex_lock(&S.lk);
    /* One clock for the whole core: our own CLOCK_MONOTONIC at processing.
     * The two wires are stamped by pipelines with different latencies (the EO
     * stamp lags the radar stamp by 20-80 ms), so using wire stamps makes the
     * tick time jump backwards on every EO frame - that was the radar-flicker
     * bug (REC 20260722T165848Z). Arrival-time skew (sub-ms on localhost) is
     * far below the association gates. */
    uint64_t t = now_ns();
    if (S.rad_frame_id && m.frame_id && m.frame_id < S.rad_frame_id)
        fus_core_sensor_reset(S.core, 0, t);          /* daemon restarted */
    S.rad_frame_id = m.frame_id;
    if (S.last_rad_ns && t > S.last_rad_ns) {
        double dt = (double)(t - S.last_rad_ns) / 1e9;
        if (dt > 1e-4) S.rad_fps += 0.1 * (1.0 / dt - S.rad_fps);
    }
    S.last_rad_ns = t;
    FusOut rows[FUS_MAX_OUT];
    int no = fus_core_step_rad(S.core, tgts, n, t, rows, FUS_MAX_OUT);
    publish_wire(rows, no);
    push_stats();
    pthread_mutex_unlock(&S.lk);
}

static void on_trk_frame(const char *json, void *user)
{
    (void)user;
    FusEoTrk trks[FUS_MAX_EO];
    TrkMeta m;
    int n = trk_parse(json, trks, FUS_MAX_EO, &m);
    pthread_mutex_lock(&S.lk);
    if (m.ifov_rad > 0) fus_core_set_eo_geom(S.core, m.img_w, m.img_h, m.ifov_rad);
    uint64_t t = now_ns();     /* one clock for the core - see on_rad_frame */
    if (S.eo_frame_id && m.frame_id && m.frame_id < S.eo_frame_id)
        fus_core_sensor_reset(S.core, 1, t);
    S.eo_frame_id = m.frame_id;
    S.eo_engaged = m.engaged;
    if (S.last_trk_ns && t > S.last_trk_ns) {
        double dt = (double)(t - S.last_trk_ns) / 1e9;
        if (dt > 1e-4) S.trk_fps += 0.1 * (1.0 / dt - S.trk_fps);
    }
    S.last_trk_ns = t;
    FusOut rows[FUS_MAX_OUT];
    int no = fus_core_step_eo(S.core, trks, n, t, rows, FUS_MAX_OUT);
    publish_wire(rows, no);
    push_stats();
    pthread_mutex_unlock(&S.lk);
}

static void on_rad_drop(void *user)
{
    (void)user;
    pthread_mutex_lock(&S.lk);
    fus_core_sensor_reset(S.core, 0, now_ns());
    S.rad_fps = 0;
    pthread_mutex_unlock(&S.lk);
}
static void on_trk_drop(void *user)
{
    (void)user;
    pthread_mutex_lock(&S.lk);
    fus_core_sensor_reset(S.core, 1, now_ns());
    S.trk_fps = 0;
    pthread_mutex_unlock(&S.lk);
}

/* ---- /ctl callback ---- */
static double g_saved_az = 1e9, g_saved_el = 1e9;
static void on_ctl(const FusCtl *c, void *user)
{
    (void)user;
    pthread_mutex_lock(&S.lk);
    FusKnobs k;
    fus_core_get_knobs(S.core, &k);
    k.trim_az = c->trim_az_deg * D2R;
    k.trim_el = c->trim_el_deg * D2R;
    k.gate = c->gate;
    k.confirm = (int)(c->confirm + 0.5);
    k.divorce_s = c->divorce_s;
    k.coast_s = c->coast_s;
    fus_core_set_knobs(S.core, &k);
    pthread_mutex_unlock(&S.lk);
    if (c->trim_az_deg != g_saved_az || c->trim_el_deg != g_saved_el) {
        if (trim_save(c->trim_az_deg, c->trim_el_deg) == 0) {
            g_saved_az = c->trim_az_deg; g_saved_el = c->trim_el_deg;
            http_set_trim_info("ctl", 0, 0, 0);
        } else
            fprintf(stderr, "fusiond: trim save failed\n");
    }
}

/* ---- 1 Hz heartbeat: emit even when both feeds are silent ---- */
static void *heartbeat_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(1000000);
        int rc = S.rad ? sse_client_connected(S.rad, 1500000000ull) : 0;
        int tc = S.trk ? sse_client_connected(S.trk, 2000000000ull) : 0;
        if (rc || tc) continue;      /* live frames are driving the wire */
        pthread_mutex_lock(&S.lk);
        FusOut rows[FUS_MAX_OUT];
        int no = fus_core_tick(S.core, now_ns(), rows, FUS_MAX_OUT);
        publish_wire(rows, no);
        push_stats();
        pthread_mutex_unlock(&S.lk);
    }
    return NULL;
}

static void usage(const char *p)
{
    fprintf(stderr, "usage: %s [-p port] [-r radar_host:port] [-t tracker_host:port]\n", p);
}

static void split_hostport(const char *arg, char *host, size_t hcap, int *port)
{
    const char *c = strchr(arg, ':');
    if (c) { snprintf(host, hcap, "%.*s", (int)(c - arg), arg); *port = atoi(c + 1); }
    else     snprintf(host, hcap, "%s", arg);
}

int main(int argc, char **argv)
{
    int port = FUS_PORT_DEFAULT;
    char rad_host[64] = RAD_STREAM_HOST; int rad_port = RAD_STREAM_PORT;
    char trk_host[64] = TRK_STREAM_HOST; int trk_port = TRK_STREAM_PORT;

    int c;
    while ((c = getopt(argc, argv, "p:r:t:h")) != -1) {
        switch (c) {
        case 'p': port = atoi(optarg); break;
        case 'r': split_hostport(optarg, rad_host, sizeof rad_host, &rad_port); break;
        case 't': split_hostport(optarg, trk_host, sizeof trk_host, &trk_port); break;
        case 'h': default: usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);      /* dropped SSE clients must not kill us */

    pthread_mutex_init(&S.lk, NULL);
    S.core = fus_core_new();
    if (!S.core) { fprintf(stderr, "fusiond: core alloc failed\n"); return 1; }
    S.eo_engaged = -1;

    /* trim: file overrides the built-in defaults; /ctl changes persist back */
    FusCtl ctl; http_get_ctl(&ctl);
    double az, el;
    if (trim_load(&az, &el)) {
        ctl.trim_az_deg = az; ctl.trim_el_deg = el;
        http_set_ctl(&ctl);
        http_set_trim_info("file", 0, 0, 0);
    } else
        http_set_trim_info("default", 0, 0, 0);
    g_saved_az = ctl.trim_az_deg; g_saved_el = ctl.trim_el_deg;
    on_ctl(&ctl, NULL);

    /* recorder tap (best-effort: a create failure just disables recording) */
    if (tap_create(&S.tap, FUS_TAP_NAME, FUS_TAP_SLOTS, FUS_TAP_BYTES,
                   "{\"name\":\"fus_wire\",\"fmt\":\"json\"}") == 0)
        S.tap_ok = 1;
    else
        fprintf(stderr, "fusiond: fus_wire tap unavailable, recording disabled\n");

    http_set_ctl_cb(on_ctl, NULL);
    if (http_start(port) != 0) { fprintf(stderr, "fusiond: http_start failed\n"); return 1; }

    S.rad = sse_client_start(rad_host, rad_port, on_rad_frame, on_rad_drop, NULL);
    S.trk = sse_client_start(trk_host, trk_port, on_trk_frame, on_trk_drop, NULL);
    if (!S.rad || !S.trk) { fprintf(stderr, "fusiond: feed start failed\n"); return 1; }

    pthread_t th_hb;
    pthread_create(&th_hb, NULL, heartbeat_thread, NULL);

    fprintf(stderr, "fusiond %s on :%d, radar %s:%d, tracker %s:%d, trim %.2f/%.2f deg\n",
            FUS_VERSION, port, rad_host, rad_port, trk_host, trk_port,
            ctl.trim_az_deg, ctl.trim_el_deg);

    pthread_join(th_hb, NULL);   /* runs forever */
    return 0;
}
