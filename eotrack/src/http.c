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

static char    *g_json = NULL;
static size_t   g_json_len = 0, g_json_cap = 0;
static uint64_t g_seq = 0;

static TrkCtl g_c = {
    .engage = -1, .gate_base = TRK_GATE_BASE_DEFAULT, .confirm = TRK_CONFIRM_DEFAULT,
    .coast_s = TRK_COAST_S_DEFAULT, .clutter_s = TRK_CLUTTER_S_DEFAULT, .lock = 1,
};
static void (*g_ctl_cb)(const TrkCtl *, void *) = NULL;
static void  *g_ctl_user = NULL;

static char   g_version[32] = TRK_VERSION;
static double g_ifov_urad = 0;
static int    g_img_w = EO_IMG_W, g_img_h = EO_IMG_H;

static struct { int det_connected, eo_tap_ok; double det_fps, out_fps; } g_feed;
static struct { int live, emitted; } g_tracks;
static struct { int engaged_tid, lock_on; double lock_score; } g_lockst;
static struct { int degraded; unsigned long err_count; char last_err[96]; } g_deg;

void http_publish(const char *json, size_t len)
{
    pthread_mutex_lock(&g_lock);
    if (len + 1 > g_json_cap) {
        size_t ncap = len + 1 + 4096;
        char *nb = realloc(g_json, ncap);
        if (nb) { g_json = nb; g_json_cap = ncap; }
    }
    if (g_json && len + 1 <= g_json_cap) {
        memcpy(g_json, json, len); g_json[len] = 0; g_json_len = len; g_seq++;
    }
    pthread_mutex_unlock(&g_lock);
}

void http_set_feed(int det_connected, int eo_tap_ok, double det_fps, double out_fps)
{
    pthread_mutex_lock(&g_lock);
    g_feed.det_connected = det_connected; g_feed.eo_tap_ok = eo_tap_ok;
    g_feed.det_fps = det_fps; g_feed.out_fps = out_fps;
    pthread_mutex_unlock(&g_lock);
}
void http_set_tracks(int live, int emitted)
{
    pthread_mutex_lock(&g_lock);
    g_tracks.live = live; g_tracks.emitted = emitted;
    pthread_mutex_unlock(&g_lock);
}
void http_set_lock(int engaged_tid, int lock_on, double lock_score)
{
    pthread_mutex_lock(&g_lock);
    g_lockst.engaged_tid = engaged_tid; g_lockst.lock_on = lock_on; g_lockst.lock_score = lock_score;
    pthread_mutex_unlock(&g_lock);
}
void http_set_degraded(int degraded, unsigned long err_count, const char *last_err)
{
    pthread_mutex_lock(&g_lock);
    g_deg.degraded = degraded; g_deg.err_count = err_count;
    snprintf(g_deg.last_err, sizeof g_deg.last_err, "%s", last_err ? last_err : "");
    pthread_mutex_unlock(&g_lock);
}
void http_set_info(const char *version, double ifov_urad, int img_w, int img_h)
{
    pthread_mutex_lock(&g_lock);
    if (version) snprintf(g_version, sizeof g_version, "%s", version);
    g_ifov_urad = ifov_urad; g_img_w = img_w; g_img_h = img_h;
    pthread_mutex_unlock(&g_lock);
}
void http_get_ctl(TrkCtl *out)
{
    pthread_mutex_lock(&g_lock); *out = g_c; pthread_mutex_unlock(&g_lock);
}
void http_set_ctl_cb(void (*cb)(const TrkCtl *, void *), void *user)
{
    g_ctl_cb = cb; g_ctl_user = user;
}

static double clampd(double v, double lo, double hi){ return v<lo?lo:v>hi?hi:v; }

static int build_stats(char *b, size_t cap)
{
    return snprintf(b, cap,
        "{\"version\":\"%s\",\"ifov_urad\":%.1f,\"img\":{\"w\":%d,\"h\":%d},"
        "\"feed\":{\"det_connected\":%s,\"eo_tap_ok\":%s,\"det_fps\":%.1f,\"out_fps\":%.1f},"
        "\"tracks\":{\"live\":%d,\"emitted\":%d},"
        "\"lock\":{\"engaged\":%d,\"on\":%s,\"score\":%.3f},"
        "\"health\":{\"degraded\":%s,\"errors\":%lu,\"last_err\":\"%s\"},"
        "\"knobs\":{\"engage\":%d,\"gate_base\":%.1f,\"confirm\":%.1f,\"coast_s\":%.2f,"
        "\"clutter_s\":%.2f,\"lock\":%d}}\n",
        g_version, g_ifov_urad, g_img_w, g_img_h,
        g_feed.det_connected ? "true" : "false", g_feed.eo_tap_ok ? "true" : "false",
        g_feed.det_fps, g_feed.out_fps,
        g_tracks.live, g_tracks.emitted,
        g_lockst.engaged_tid, g_lockst.lock_on ? "true" : "false", g_lockst.lock_score,
        g_deg.degraded ? "true" : "false", g_deg.err_count, g_deg.last_err,
        g_c.engage, g_c.gate_base, g_c.confirm, g_c.coast_s, g_c.clutter_s, g_c.lock);
}

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;
    /* Only ever parse the request line (up to the first CRLF), never headers -
     * a knob name in a header value must not be mistaken for a query param. */
    char *eol = strstr(req, "\r\n"); if (eol) *eol = 0;

    if (!strncmp(req, "GET /stats", 10)) {
        pthread_mutex_lock(&g_lock);
        char body[1536]; int bl = build_stats(body, sizeof body);
        pthread_mutex_unlock(&g_lock);
        if (bl < 0) bl = 0;
        if ((size_t)bl >= sizeof body) bl = (int)sizeof body - 1;
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
        close(fd); return NULL;
    }

    if (!strncmp(req, "GET /ctl", 8)) {
        char *q;
        pthread_mutex_lock(&g_lock); TrkCtl k = g_c; pthread_mutex_unlock(&g_lock);
        if ((q = strstr(req, "engage=")))    k.engage = atoi(q + 7);
        if ((q = strstr(req, "gate_base=")))  k.gate_base = atof(q + 10);
        if ((q = strstr(req, "confirm=")))    k.confirm = atof(q + 8);
        if ((q = strstr(req, "coast_s=")))    k.coast_s = atof(q + 8);
        if ((q = strstr(req, "clutter_s=")))  k.clutter_s = atof(q + 10);
        if ((q = strstr(req, "lock=")))       k.lock = atoi(q + 5) ? 1 : 0;

        k.gate_base = clampd(k.gate_base, TRK_GATE_BASE_MIN, TRK_GATE_BASE_MAX);
        k.confirm   = clampd(k.confirm,   TRK_CONFIRM_MIN,   TRK_CONFIRM_MAX);
        k.coast_s   = clampd(k.coast_s,   TRK_COAST_S_MIN,   TRK_COAST_S_MAX);
        k.clutter_s = clampd(k.clutter_s, TRK_CLUTTER_S_MIN, TRK_CLUTTER_S_MAX);

        pthread_mutex_lock(&g_lock); g_c = k; pthread_mutex_unlock(&g_lock);
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
            last = g_seq; size_t len = g_json_len;
            char *copy = malloc(len + 1);
            if (copy) { memcpy(copy, g_json, len); copy[len] = 0; }
            pthread_mutex_unlock(&g_lock);
            if (!copy) break;
            if (dprintf(fd, "data: %s\n\n", copy) < 0) { free(copy); break; }
            free(copy);
        }
        close(fd); return NULL;
    }

    const char *body = "trackerd - endpoints: /stream (SSE), /stats, /ctl\n";
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
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("trackerd http: bind"); return NULL; }
    listen(s, 8);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        /* per-client socket timeouts so one idle client can never wedge a thread */
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
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
