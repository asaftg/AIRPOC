/* Radar SSE client — subscribes to the radar daemon's /stream (radar/ module, :8092)
 * and keeps the latest frame JSON. See radar.h. Reconnects forever; reports
 * connected=0 while the daemon is unreachable (the app must run fine with no radar).
 * The daemon and the app both run on the Jetson, so this is a localhost hop. */
#define _GNU_SOURCE
#include "radar.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ACC_CAP (128 * 1024)

static pthread_t       th;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static char           *g_json = NULL;      /* latest frame JSON (grown as needed) */
static int             g_len = 0, g_cap = 0;
static volatile int    g_connected = 0;    /* daemon reports a radar connected     */
static volatile int    g_ntargets = 0;
static char            g_host[96] = "127.0.0.1";
static int             g_port = 8092;

static void nap(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static int connect_daemon(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* Store one frame; extract connected + num_targets for /stats (cheap substring). */
static void store_frame(const char *json, int len)
{
    int conn = strstr(json, "\"connected\":true") != NULL;
    int nt = 0;
    const char *p = strstr(json, "\"num_targets\":");
    if (p) nt = atoi(p + 14);
    pthread_mutex_lock(&lk);
    if (len + 1 > g_cap) { char *nb = realloc(g_json, len + 1 + 4096); if (nb) { g_json = nb; g_cap = len + 1 + 4096; } }
    if (g_json && len + 1 <= g_cap) { memcpy(g_json, json, len); g_json[len] = 0; g_len = len; }
    g_connected = conn; g_ntargets = nt;
    pthread_mutex_unlock(&lk);
}

/* Read one SSE session: send GET /stream, skip headers, extract `data: <json>` lines
 * until the socket closes. Returns when the connection ends. */
static void run_session(int fd)
{
    const char *req = "GET /stream HTTP/1.0\r\nHost: radar\r\nAccept: text/event-stream\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) return;

    char *acc = malloc(ACC_CAP);
    if (!acc) return;
    int len = 0, hdr_done = 0;

    for (;;) {
        if (len >= ACC_CAP - 1) len = 0;                 /* overlong line: drop, resync */
        ssize_t n = read(fd, acc + len, ACC_CAP - 1 - len);
        if (n <= 0) break;
        len += (int)n; acc[len] = 0;

        if (!hdr_done) {
            char *e = strstr(acc, "\r\n\r\n");
            if (!e) continue;
            int off = (int)(e - acc) + 4;
            memmove(acc, acc + off, len - off); len -= off; acc[len] = 0;
            hdr_done = 1;
        }
        /* consume complete '\n'-terminated lines */
        char *nl;
        while ((nl = memchr(acc, '\n', len))) {
            int ll = (int)(nl - acc);
            if (ll > 0 && acc[ll - 1] == '\r') acc[ll - 1] = 0; else acc[ll] = 0;
            if (!strncmp(acc, "data: ", 6)) store_frame(acc + 6, (int)strlen(acc + 6));
            int adv = ll + 1;
            memmove(acc, acc + adv, len - adv); len -= adv; acc[len] = 0;
        }
    }
    free(acc);
}

static void *reader(void *a)
{
    (void)a;
    while (run_flag) {
        int fd = connect_daemon();
        if (fd < 0) { pthread_mutex_lock(&lk); g_connected = 0; pthread_mutex_unlock(&lk); nap(1000); continue; }
        run_session(fd);
        close(fd);
        pthread_mutex_lock(&lk); g_connected = 0; g_ntargets = 0; pthread_mutex_unlock(&lk);
        if (run_flag) nap(500);
    }
    return NULL;
}

int radar_start(const char *host_port)
{
    if (host_port && *host_port) {
        char tmp[80]; snprintf(tmp, sizeof tmp, "%s", host_port);
        char *c = strchr(tmp, ':');
        if (c) { *c = 0; g_port = atoi(c + 1); }
        snprintf(g_host, sizeof g_host, "%s", tmp);
    }
    run_flag = 1;
    if (pthread_create(&th, NULL, reader, NULL) != 0) { run_flag = 0; return -1; }
    fprintf(stderr, "radar: consuming daemon SSE at %s:%d/stream\n", g_host, g_port);
    return 0;
}

void radar_stop(void)
{
    if (run_flag) { run_flag = 0; pthread_join(th, NULL); }
    free(g_json); g_json = NULL; g_cap = g_len = 0;
}

int radar_get_frame_json(char *buf, int cap)
{
    pthread_mutex_lock(&lk);
    int n = 0;
    if (g_json && g_len > 0 && g_len < cap) { memcpy(buf, g_json, g_len); buf[g_len] = 0; n = g_len; }
    pthread_mutex_unlock(&lk);
    return n;
}

int radar_connected(void)   { return g_connected; }
int radar_num_targets(void) { return g_ntargets; }

/* Best-effort forward of cluster cfg to the daemon's control endpoint (if present). */
void radar_set_tune(double cluster_eps_m, int min_points)
{
    int fd = connect_daemon();
    if (fd < 0) return;
    char req[160];
    int n = snprintf(req, sizeof req,
        "GET /ctl?eps=%.2f&minpts=%d HTTP/1.0\r\nHost: radar\r\n\r\n",
        cluster_eps_m, min_points);
    ssize_t w = write(fd, req, n); (void)w;
    close(fd);
}
