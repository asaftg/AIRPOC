#define _GNU_SOURCE
#include "http.h"
#include "cluster.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char          *g_json = NULL;      /* latest frame JSON */
static size_t         g_json_len = 0;
static size_t         g_json_cap = 0;
static uint64_t       g_seq = 0;
static char           g_webroot[512] = "web";

/* Live DBSCAN controls, set via /ctl and echoed in /stats so GUI sliders can
 * initialise. The actual clustering values live in the RadarClusterer; the
 * registered callback pushes changes there. */
static void (*g_ctl_cb)(double, int, double, double, double, double, double, int, double, double, void *) = NULL;
static void  *g_ctl_user = NULL;
static double g_eps    = CLUSTER_DEFAULT_EPS_M;
static int    g_minpts = CLUSTER_DEFAULT_MIN_PTS;
static double g_speed  = CLUSTER_DEFAULT_SPEED;
static double g_snrmin = CLUSTER_DEFAULT_SNR;
static double g_fov    = CLUSTER_DEFAULT_FOV;
static double g_elmax  = CLUSTER_DEFAULT_ELMAX;
static double g_dop    = CLUSTER_DEFAULT_DOP;
static int    g_confirm = CLUSTER_DEFAULT_CONFIRM;
static double g_coast  = CLUSTER_DEFAULT_COAST_S;
static double g_park   = CLUSTER_DEFAULT_PARK_S;

void http_set_ctl_cb(void (*cb)(double, int, double, double, double, double, double, int, double, double, void *), void *user) {
    g_ctl_cb = cb; g_ctl_user = user;
}

static struct {
    double fps, max_range_m, fov_half_deg;
    unsigned long drops;
    int n_points, n_targets, connected;
    char profile[64];
    /* chip-reported per-frame DSP timing (TLV 6); have_timing=0 until seen */
    double dsp_proc_us, dsp_margin_us, active_cpu_pct, interframe_cpu_pct;
    int have_timing;
    /* tracker patience-chain counters (far-range chain detector) */
    int chains_active;
    unsigned long chains_total;
} g_stat;

void http_publish(const char *json, size_t len) {
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

void http_set_stats(double fps, unsigned long drops, int n_points,
                    int n_targets, int connected, const char *profile,
                    double max_range_m, double fov_half_deg) {
    pthread_mutex_lock(&g_lock);
    g_stat.fps = fps; g_stat.drops = drops;
    g_stat.n_points = n_points; g_stat.n_targets = n_targets;
    g_stat.connected = connected;
    g_stat.max_range_m = max_range_m; g_stat.fov_half_deg = fov_half_deg;
    snprintf(g_stat.profile, sizeof(g_stat.profile), "%s", profile ? profile : "");
    pthread_mutex_unlock(&g_lock);
}

void http_set_timing(double dsp_proc_us, double dsp_margin_us,
                     double active_cpu_pct, double interframe_cpu_pct) {
    pthread_mutex_lock(&g_lock);
    g_stat.dsp_proc_us = dsp_proc_us;
    g_stat.dsp_margin_us = dsp_margin_us;
    g_stat.active_cpu_pct = active_cpu_pct;
    g_stat.interframe_cpu_pct = interframe_cpu_pct;
    g_stat.have_timing = 1;
    pthread_mutex_unlock(&g_lock);
}

void http_set_chain_stats(int chains_active,
                          unsigned long chains_confirmed_total) {
    pthread_mutex_lock(&g_lock);
    g_stat.chains_active = chains_active;
    g_stat.chains_total = chains_confirmed_total;
    pthread_mutex_unlock(&g_lock);
}

static const char *ctype(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".css"))  return "text/css";
    return "application/octet-stream";
}

/* Serve webroot/<name>. name must not contain "..". */
static void serve_file(int fd, const char *name) {
    if (strstr(name, "..")) { dprintf(fd, "HTTP/1.0 400 Bad Request\r\n\r\n"); return; }
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", g_webroot, name);
    FILE *f = fopen(path, "rb");
    if (!f) { dprintf(fd, "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *body = malloc(sz > 0 ? (size_t)sz : 1);
    size_t rd = body ? fread(body, 1, (size_t)sz, f) : 0;
    fclose(f);
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                "Connection: close\r\n\r\n", ctype(name), rd);
    ssize_t w = write(fd, body, rd); (void)w;
    free(body);
}

static void *client(void *arg) {
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    if (!strncmp(req, "GET /stats", 10)) {
        pthread_mutex_lock(&g_lock);
        char body[1024];
        int bl = snprintf(body, sizeof(body),
            "{\"fps\":%.1f,\"drops\":%lu,\"num_points\":%d,\"num_targets\":%d,"
            "\"connected\":%s,\"profile\":\"%s\",\"max_range_m\":%.1f,"
            "\"cluster_eps_m\":%.2f,\"cluster_min_pts\":%d,"
            "\"speed_min_mps\":%.2f,\"snr_min_db\":%.1f,"
            "\"fov_half_deg\":%.1f,\"el_max_deg\":%.1f,\"doppler_gate_mps\":%.2f,"
            "\"confirm\":%d,\"coast_s\":%.2f,\"park_s\":%.1f,"
            "\"dsp_valid\":%s,\"dsp_proc_ms\":%.3f,\"dsp_margin_ms\":%.3f,"
            "\"active_cpu_pct\":%.0f,\"interframe_cpu_pct\":%.0f,"
            "\"chains_active\":%d,\"chains_confirmed_total\":%lu}\n",
            g_stat.fps, g_stat.drops, g_stat.n_points, g_stat.n_targets,
            g_stat.connected ? "true" : "false", g_stat.profile,
            g_stat.max_range_m, g_eps, g_minpts,
            g_speed, g_snrmin, g_fov, g_elmax, g_dop,
            g_confirm, g_coast, g_park,
            g_stat.have_timing ? "true" : "false",
            g_stat.dsp_proc_us / 1000.0, g_stat.dsp_margin_us / 1000.0,
            g_stat.active_cpu_pct, g_stat.interframe_cpu_pct,
            g_stat.chains_active, g_stat.chains_total);
        pthread_mutex_unlock(&g_lock);
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
        close(fd); return NULL;
    }

    /* GET /ctl?eps=&minpts=&speed=&snrmin=&fov=&elmax=&doppler=&confirm=&coast=&park=
     * — set the live tracker knobs. Absent params keep their current value.
     * Always 200 OK. */
    if (!strncmp(req, "GET /ctl", 8)) {
        char *q;
        double eps = g_eps; int mp = g_minpts;
        double spd = g_speed, snr = g_snrmin, fov = g_fov, elm = g_elmax, dop = g_dop;
        int cfm = g_confirm; double cst = g_coast, prk = g_park;
        if ((q = strstr(req, "eps=")))     eps = atof(q + 4);
        if ((q = strstr(req, "minpts=")))  mp  = atoi(q + 7);
        if ((q = strstr(req, "speed=")))   spd = atof(q + 6);
        if ((q = strstr(req, "snrmin=")))  snr = atof(q + 7);
        if ((q = strstr(req, "fov=")))     fov = atof(q + 4);
        if ((q = strstr(req, "elmax=")))   elm = atof(q + 6);
        if ((q = strstr(req, "doppler="))) dop = atof(q + 8);
        if ((q = strstr(req, "confirm="))) cfm = atoi(q + 8);
        if ((q = strstr(req, "coast=")))   cst = atof(q + 6);
        if ((q = strstr(req, "park=")))    prk = atof(q + 5);
        /* Clamp to the same bounds the clusterer uses, so /stats echoes the
         * value actually applied (not a raw out-of-range request). */
        if (eps < CLUSTER_EPS_MIN_M) eps = CLUSTER_EPS_MIN_M;
        if (eps > CLUSTER_EPS_MAX_M) eps = CLUSTER_EPS_MAX_M;
        if (mp < CLUSTER_MIN_PTS_MIN) mp = CLUSTER_MIN_PTS_MIN;
        if (mp > CLUSTER_MIN_PTS_MAX) mp = CLUSTER_MIN_PTS_MAX;
        if (spd < CLUSTER_SPEED_MIN) spd = CLUSTER_SPEED_MIN;
        if (spd > CLUSTER_SPEED_MAX) spd = CLUSTER_SPEED_MAX;
        if (snr < CLUSTER_SNR_MIN) snr = CLUSTER_SNR_MIN;
        if (snr > CLUSTER_SNR_MAX) snr = CLUSTER_SNR_MAX;
        if (fov < CLUSTER_FOV_MIN) fov = CLUSTER_FOV_MIN;
        if (fov > CLUSTER_FOV_MAX) fov = CLUSTER_FOV_MAX;
        if (elm < CLUSTER_ELMAX_MIN) elm = CLUSTER_ELMAX_MIN;
        if (elm > CLUSTER_ELMAX_MAX) elm = CLUSTER_ELMAX_MAX;
        if (dop < CLUSTER_DOP_MIN) dop = CLUSTER_DOP_MIN;
        if (dop > CLUSTER_DOP_MAX) dop = CLUSTER_DOP_MAX;
        if (cfm < CLUSTER_CONFIRM_MIN) cfm = CLUSTER_CONFIRM_MIN;
        if (cfm > CLUSTER_CONFIRM_MAX) cfm = CLUSTER_CONFIRM_MAX;
        if (cst < CLUSTER_COAST_MIN) cst = CLUSTER_COAST_MIN;
        if (cst > CLUSTER_COAST_MAX) cst = CLUSTER_COAST_MAX;
        if (prk < CLUSTER_PARK_MIN) prk = CLUSTER_PARK_MIN;
        if (prk > CLUSTER_PARK_MAX) prk = CLUSTER_PARK_MAX;
        pthread_mutex_lock(&g_lock);
        g_eps = eps; g_minpts = mp; g_speed = spd; g_snrmin = snr; g_fov = fov; g_elmax = elm; g_dop = dop;
        g_confirm = cfm; g_coast = cst; g_park = prk;
        pthread_mutex_unlock(&g_lock);
        if (g_ctl_cb) g_ctl_cb(eps, mp, spd, snr, fov, elm, dop, cfm, cst, prk, g_ctl_user);
        const char *ok = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                         "Content-Length: 2\r\nConnection: close\r\n\r\nok";
        ssize_t w = write(fd, ok, strlen(ok)); (void)w;
        close(fd); return NULL;
    }

    if (!strncmp(req, "GET /stream", 11)) {
        /* Server-Sent Events: one JSON frame per new sequence number. */
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

    /* "GET / ..." -> index.html; "GET /radar_view.js" -> that file. */
    char name[256] = "index.html";
    if (!strncmp(req, "GET /", 5)) {
        const char *p = req + 5;
        if (*p != ' ') {
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '?' && i < sizeof(name) - 1) { name[i] = p[i]; i++; }
            name[i] = 0;
        }
    }
    serve_file(fd, name);
    close(fd);
    return NULL;
}

static void *server(void *arg) {
    int port = (int)(long)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("radar http: bind"); return NULL; }
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

int http_start(int port, const char *webroot) {
    if (webroot && *webroot) snprintf(g_webroot, sizeof(g_webroot), "%s", webroot);
    pthread_t t;
    if (pthread_create(&t, NULL, server, (void *)(long)port)) return -1;
    pthread_detach(t);
    return 0;
}
