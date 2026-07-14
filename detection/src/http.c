#define _GNU_SOURCE
#include "http.h"
#include "config.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* latest SSE frame */
static char    *g_json = NULL;
static size_t   g_json_len = 0, g_json_cap = 0;
static uint64_t g_seq = 0;

/* live knobs (owned here so /stats echoes what was applied) */
static DetKnobs g_k = {
    .conf = DET_CONF_DEFAULT, .cadence = DET_CADENCE_DEFAULT,
    .motion = DET_MOTION_DEFAULT, .max_dets = DET_MAXDETS_DEFAULT,
    .nms = DET_NMS_DEFAULT,
    .mot_k = DET_MOT_K_DEFAULT, .mot_window_s = DET_MOT_WINDOW_S_DEFAULT,
    .mot_persist = DET_MOT_PERSIST_DEFAULT,
    .mot_down = DET_MOT_DOWN_DEFAULT, .mot_fps = DET_MOT_FPS_DEFAULT,
};
static void (*g_ctl_cb)(const DetKnobs *, void *) = NULL;
static void  *g_ctl_user = NULL;

/* static info */
static char   g_version[32] = DET_VERSION;
static double g_ifov_urad = 0;
static int    g_img_w = EO_IMG_W, g_img_h = EO_IMG_H;

/* health snapshots */
static struct {
    int connected; double fps; unsigned long gaps, drops_cum;
    unsigned long long frame_id;
} g_tap;
static struct {
    int active; double fps, infer_p50, infer_p95, e2e_p50, e2e_p95;
    char model[64], precision[16];
} g_det;
static struct {
    int active; double fps, stab_fail_pct; int candidates;
} g_mot;

void http_publish(const char *json, size_t len)
{
    pthread_mutex_lock(&g_lock);
    if (len + 1 > g_json_cap) {
        size_t ncap = len + 1 + 4096;
        char *nb = realloc(g_json, ncap);
        if (nb) { g_json = nb; g_json_cap = ncap; }
    }
    if (g_json && len + 1 <= g_json_cap) {
        memcpy(g_json, json, len);
        g_json[len] = 0;
        g_json_len = len;
        g_seq++;
    }
    pthread_mutex_unlock(&g_lock);
}

void http_set_tap(int connected, double fps, unsigned long gaps,
                  unsigned long drops_cum, unsigned long long frame_id)
{
    pthread_mutex_lock(&g_lock);
    g_tap.connected = connected; g_tap.fps = fps; g_tap.gaps = gaps;
    g_tap.drops_cum = drops_cum; g_tap.frame_id = frame_id;
    pthread_mutex_unlock(&g_lock);
}

void http_set_det(double fps, double infer_ms_p50, double infer_ms_p95,
                  double e2e_ms_p50, double e2e_ms_p95,
                  const char *model, const char *precision)
{
    pthread_mutex_lock(&g_lock);
    g_det.active = 1; g_det.fps = fps;
    g_det.infer_p50 = infer_ms_p50; g_det.infer_p95 = infer_ms_p95;
    g_det.e2e_p50 = e2e_ms_p50; g_det.e2e_p95 = e2e_ms_p95;
    snprintf(g_det.model, sizeof g_det.model, "%s", model ? model : "none");
    snprintf(g_det.precision, sizeof g_det.precision, "%s", precision ? precision : "");
    pthread_mutex_unlock(&g_lock);
}

void http_set_motion(double fps, double stab_fail_pct, int candidates)
{
    pthread_mutex_lock(&g_lock);
    g_mot.active = 1; g_mot.fps = fps;
    g_mot.stab_fail_pct = stab_fail_pct; g_mot.candidates = candidates;
    pthread_mutex_unlock(&g_lock);
}

void http_set_info(const char *version, double ifov_urad, int img_w, int img_h)
{
    pthread_mutex_lock(&g_lock);
    if (version) snprintf(g_version, sizeof g_version, "%s", version);
    g_ifov_urad = ifov_urad; g_img_w = img_w; g_img_h = img_h;
    pthread_mutex_unlock(&g_lock);
}

void http_get_knobs(DetKnobs *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_k;
    pthread_mutex_unlock(&g_lock);
}

void http_set_ctl_cb(void (*cb)(const DetKnobs *, void *), void *user)
{
    g_ctl_cb = cb; g_ctl_user = user;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static int    clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int build_stats(char *b, size_t cap)
{
    return snprintf(b, cap,
        "{\"version\":\"%s\",\"ifov_urad\":%.1f,\"img\":{\"w\":%d,\"h\":%d},"
        "\"tap\":{\"connected\":%s,\"fps\":%.1f,\"gaps\":%lu,\"drops_cum\":%lu,"
        "\"frame_id\":%llu},"
        "\"det\":{\"active\":%s,\"fps\":%.1f,\"model\":\"%s\",\"precision\":\"%s\","
        "\"infer_ms\":{\"p50\":%.2f,\"p95\":%.2f},\"e2e_ms\":{\"p50\":%.2f,\"p95\":%.2f}},"
        "\"motion\":{\"active\":%s,\"fps\":%.1f,\"stab_fail_pct\":%.1f,\"candidates\":%d},"
        "\"knobs\":{\"conf\":%.2f,\"cadence\":%d,\"motion\":%d,\"max_dets\":%d,"
        "\"nms\":%.2f,\"mot_k\":%.1f,\"mot_window_s\":%.1f,\"mot_persist\":%d,"
        "\"mot_down\":%d,\"mot_fps\":%d}}\n",
        g_version, g_ifov_urad, g_img_w, g_img_h,
        g_tap.connected ? "true" : "false", g_tap.fps, g_tap.gaps, g_tap.drops_cum,
        g_tap.frame_id,
        g_det.active ? "true" : "false", g_det.fps, g_det.model, g_det.precision,
        g_det.infer_p50, g_det.infer_p95, g_det.e2e_p50, g_det.e2e_p95,
        g_mot.active ? "true" : "false", g_mot.fps, g_mot.stab_fail_pct, g_mot.candidates,
        g_k.conf, g_k.cadence, g_k.motion, g_k.max_dets, g_k.nms, g_k.mot_k,
        g_k.mot_window_s, g_k.mot_persist, g_k.mot_down, g_k.mot_fps);
}

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    if (!strncmp(req, "GET /stats", 10)) {
        pthread_mutex_lock(&g_lock);
        char body[1024];
        int bl = build_stats(body, sizeof body);
        pthread_mutex_unlock(&g_lock);
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
        close(fd); return NULL;
    }

    /* GET /ctl?conf=&cadence=&motion=&max_dets=&mot_k=&mot_window_s=&mot_persist=
     * Absent params keep their current value. Clamped server-side. Always 200. */
    if (!strncmp(req, "GET /ctl", 8)) {
        char *q;
        pthread_mutex_lock(&g_lock);
        DetKnobs k = g_k;
        pthread_mutex_unlock(&g_lock);
        if ((q = strstr(req, "conf=")))        k.conf = atof(q + 5);
        if ((q = strstr(req, "cadence=")))     k.cadence = atoi(q + 8);
        if ((q = strstr(req, "motion=")))      k.motion = atoi(q + 7);
        if ((q = strstr(req, "max_dets=")))    k.max_dets = atoi(q + 9);
        if ((q = strstr(req, "nms=")))         k.nms = atof(q + 4);
        if ((q = strstr(req, "mot_k=")))       k.mot_k = atof(q + 6);
        if ((q = strstr(req, "mot_window_s="))) k.mot_window_s = atof(q + 13);
        if ((q = strstr(req, "mot_persist=")))  k.mot_persist = atoi(q + 12);
        if ((q = strstr(req, "mot_down=")))     k.mot_down = atoi(q + 9);
        if ((q = strstr(req, "mot_fps=")))      k.mot_fps = atoi(q + 8);
        k.conf = clampd(k.conf, DET_CONF_MIN, DET_CONF_MAX);
        k.cadence = clampi(k.cadence, DET_CADENCE_MIN, DET_CADENCE_MAX);
        k.motion = k.motion ? 1 : 0;
        k.max_dets = clampi(k.max_dets, DET_MAXDETS_MIN, DET_MAXDETS_MAX);
        k.nms = clampd(k.nms, DET_NMS_MIN, DET_NMS_MAX);
        k.mot_k = clampd(k.mot_k, DET_MOT_K_MIN, DET_MOT_K_MAX);
        k.mot_window_s = clampd(k.mot_window_s, DET_MOT_WINDOW_S_MIN, DET_MOT_WINDOW_S_MAX);
        k.mot_persist = clampi(k.mot_persist, DET_MOT_PERSIST_MIN, DET_MOT_PERSIST_MAX);
        k.mot_down = clampi(k.mot_down, DET_MOT_DOWN_MIN, DET_MOT_DOWN_MAX);
        k.mot_fps = clampi(k.mot_fps, DET_MOT_FPS_MIN, DET_MOT_FPS_MAX);
        pthread_mutex_lock(&g_lock);
        g_k = k;
        pthread_mutex_unlock(&g_lock);
        if (g_ctl_cb) g_ctl_cb(&k, g_ctl_user);
        const char *ok = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                         "Content-Length: 2\r\nConnection: close\r\n\r\nok";
        ssize_t w = write(fd, ok, strlen(ok)); (void)w;
        close(fd); return NULL;
    }

    if (!strncmp(req, "GET /stream", 11)) {
        const char *hdr = "HTTP/1.0 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
        if (write(fd, hdr, strlen(hdr)) < 0) { close(fd); return NULL; }
        uint64_t last = 0;
        for (;;) {
            pthread_mutex_lock(&g_lock);
            if (g_seq == last || !g_json) { pthread_mutex_unlock(&g_lock); usleep(3000); continue; }
            last = g_seq;
            size_t len = g_json_len;
            char *copy = malloc(len + 1);
            if (copy) { memcpy(copy, g_json, len); copy[len] = 0; }
            pthread_mutex_unlock(&g_lock);
            if (!copy) break;
            if (dprintf(fd, "data: %s\n\n", copy) < 0) { free(copy); break; }
            free(copy);
        }
        close(fd); return NULL;
    }

    /* Minimal root so a browser hit isn't a 404; no static assets served here. */
    const char *body = "detectiond — endpoints: /stream (SSE), /stats, /ctl\n";
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(body), body);
    close(fd);
    return NULL;
}

static void *server(void *arg)
{
    int port = (int)(long)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("detection http: bind"); return NULL; }
    listen(s, 8);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, client, (void *)(long)fd);
        pthread_detach(t);
    }
    return NULL;
}

int http_start(int port)
{
    pthread_t t;
    if (pthread_create(&t, NULL, server, (void *)(long)port)) return -1;
    pthread_detach(t);
    return 0;
}
