/* http.c - fusiond's server (eotrack http.c adapted: request-line-only parse,
 * clamped /ctl always answering "ok", knobs discoverable via /stats, per-client
 * SSE pusher threads on a seq counter, socket timeouts so an idle client can
 * never wedge a thread).
 */
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

static FusCtl g_c = {
    .trim_az_deg = FUS_TRIM_AZ_DEG_DEFAULT, .trim_el_deg = FUS_TRIM_EL_DEG_DEFAULT,
    .gate = FUS_GATE_SCALE_DEFAULT, .confirm = FUS_CONFIRM_DEFAULT,
    .divorce_s = FUS_DIVORCE_S_DEFAULT, .coast_s = FUS_COAST_S_DEFAULT,
};
static void (*g_ctl_cb)(const FusCtl *, void *) = NULL;
static void  *g_ctl_user = NULL;

static struct { int rad, trk; double rad_fps, trk_fps, out_fps; } g_feeds;
static struct { int fused, eo_only, rad_only; } g_tracks;
static struct { char source[16]; double est_az, est_el; int est_n; } g_trim = { "default", 0, 0, 0 };
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

void http_set_feeds(int rad, int trk, double rad_fps, double trk_fps, double out_fps)
{
    pthread_mutex_lock(&g_lock);
    g_feeds.rad = rad; g_feeds.trk = trk;
    g_feeds.rad_fps = rad_fps; g_feeds.trk_fps = trk_fps; g_feeds.out_fps = out_fps;
    pthread_mutex_unlock(&g_lock);
}
void http_set_tracks(int fused, int eo_only, int rad_only)
{
    pthread_mutex_lock(&g_lock);
    g_tracks.fused = fused; g_tracks.eo_only = eo_only; g_tracks.rad_only = rad_only;
    pthread_mutex_unlock(&g_lock);
}
void http_set_trim_info(const char *source, double est_az_deg, double est_el_deg, int est_n)
{
    pthread_mutex_lock(&g_lock);
    if (source) snprintf(g_trim.source, sizeof g_trim.source, "%s", source);
    g_trim.est_az = est_az_deg; g_trim.est_el = est_el_deg; g_trim.est_n = est_n;
    pthread_mutex_unlock(&g_lock);
}
void http_set_degraded(int degraded, unsigned long err_count, const char *last_err)
{
    pthread_mutex_lock(&g_lock);
    g_deg.degraded = degraded; g_deg.err_count = err_count;
    snprintf(g_deg.last_err, sizeof g_deg.last_err, "%s", last_err ? last_err : "");
    pthread_mutex_unlock(&g_lock);
}
void http_get_ctl(FusCtl *out)
{
    pthread_mutex_lock(&g_lock); *out = g_c; pthread_mutex_unlock(&g_lock);
}
void http_set_ctl(const FusCtl *c)
{
    pthread_mutex_lock(&g_lock); g_c = *c; pthread_mutex_unlock(&g_lock);
}
void http_set_ctl_cb(void (*cb)(const FusCtl *, void *), void *user)
{
    g_ctl_cb = cb; g_ctl_user = user;
}

static double clampd(double v, double lo, double hi){ return v<lo?lo:v>hi?hi:v; }

static int build_stats(char *b, size_t cap)
{
    return snprintf(b, cap,
        "{\"version\":\"%s\","
        "\"feeds\":{\"rad_connected\":%s,\"trk_connected\":%s,"
        "\"rad_fps\":%.1f,\"trk_fps\":%.1f,\"out_fps\":%.1f},"
        "\"tracks\":{\"fused\":%d,\"eo_only\":%d,\"rad_only\":%d},"
        "\"trim\":{\"az_deg\":%.2f,\"el_deg\":%.2f,\"source\":\"%s\","
        "\"est_az_deg\":%.2f,\"est_el_deg\":%.2f,\"est_n\":%d},"
        "\"health\":{\"degraded\":%s,\"errors\":%lu,\"last_err\":\"%s\"},"
        "\"knobs\":{\"trim_az\":%.2f,\"trim_el\":%.2f,\"gate\":%.2f,"
        "\"confirm\":%.0f,\"divorce_s\":%.2f,\"coast_s\":%.2f}}\n",
        FUS_VERSION,
        g_feeds.rad ? "true" : "false", g_feeds.trk ? "true" : "false",
        g_feeds.rad_fps, g_feeds.trk_fps, g_feeds.out_fps,
        g_tracks.fused, g_tracks.eo_only, g_tracks.rad_only,
        g_c.trim_az_deg, g_c.trim_el_deg, g_trim.source,
        g_trim.est_az, g_trim.est_el, g_trim.est_n,
        g_deg.degraded ? "true" : "false", g_deg.err_count, g_deg.last_err,
        g_c.trim_az_deg, g_c.trim_el_deg, g_c.gate,
        g_c.confirm, g_c.divorce_s, g_c.coast_s);
}

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;
    /* only ever parse the request line - never headers */
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
        pthread_mutex_lock(&g_lock); FusCtl k = g_c; pthread_mutex_unlock(&g_lock);
        if ((q = strstr(req, "trim_az=")))   k.trim_az_deg = atof(q + 8);
        if ((q = strstr(req, "trim_el=")))   k.trim_el_deg = atof(q + 8);
        if ((q = strstr(req, "gate=")))      k.gate = atof(q + 5);
        if ((q = strstr(req, "confirm=")))   k.confirm = atof(q + 8);
        if ((q = strstr(req, "divorce_s="))) k.divorce_s = atof(q + 10);
        if ((q = strstr(req, "coast_s=")))   k.coast_s = atof(q + 8);

        k.trim_az_deg = clampd(k.trim_az_deg, -FUS_TRIM_ABS_MAX_DEG, FUS_TRIM_ABS_MAX_DEG);
        k.trim_el_deg = clampd(k.trim_el_deg, -FUS_TRIM_ABS_MAX_DEG, FUS_TRIM_ABS_MAX_DEG);
        k.gate      = clampd(k.gate,      FUS_GATE_SCALE_MIN, FUS_GATE_SCALE_MAX);
        k.confirm   = clampd(k.confirm,   FUS_CONFIRM_MIN,    FUS_CONFIRM_MAX);
        k.divorce_s = clampd(k.divorce_s, FUS_DIVORCE_S_MIN,  FUS_DIVORCE_S_MAX);
        k.coast_s   = clampd(k.coast_s,   FUS_COAST_S_MIN,    FUS_COAST_S_MAX);

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

    const char *body = "fusiond - endpoints: /stream (SSE), /stats, /ctl\n";
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
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("fusiond http: bind"); return NULL; }
    listen(s, 8);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
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
