#define _GNU_SOURCE
#include "http.h"
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

static struct {
    double fps, max_range_m, fov_half_deg;
    unsigned long drops;
    int n_points, n_targets, connected;
    char profile[64];
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
        char body[320];
        int bl = snprintf(body, sizeof(body),
            "{\"fps\":%.1f,\"drops\":%lu,\"num_points\":%d,\"num_targets\":%d,"
            "\"connected\":%s,\"profile\":\"%s\",\"max_range_m\":%.1f,"
            "\"fov_half_deg\":%.1f}\n",
            g_stat.fps, g_stat.drops, g_stat.n_points, g_stat.n_targets,
            g_stat.connected ? "true" : "false", g_stat.profile,
            g_stat.max_range_m, g_stat.fov_half_deg);
        pthread_mutex_unlock(&g_lock);
        dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
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
