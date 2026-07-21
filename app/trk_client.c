/* EO-tracker SSE client — subscribes to the EO tracker daemon's /stream (eotrack/
 * module, trackerd on :8095) and keeps the latest track message. See trk.h. Reconnects
 * forever; reports connected=0 while the daemon is unreachable (the app must run fine with
 * no tracker). Same structure as det_client.c / radar_client.c — localhost SSE consumers. */
#define _GNU_SOURCE
#include "trk.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ACC_CAP   (256 * 1024)   /* track messages carry the live track set */
#define STATS_CAP 8192           /* daemon /stats is a nested object, not a flat line */

static pthread_t       th;
static pthread_t       stats_th;
static int             stats_th_ok = 0;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;   /* signalled on every message */
static char           *g_json = NULL;
static int             g_len = 0, g_cap = 0;
static unsigned        g_seq = 0;
static volatile int    g_connected = 0;    /* SSE session to the daemon is up */
static char            g_tstats[STATS_CAP] = "";
static char            g_host[96] = "127.0.0.1";
static int             g_port = 8095;

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

static void store_msg(const char *json, int len)
{
    pthread_mutex_lock(&lk);
    if (len + 1 > g_cap) { char *nb = realloc(g_json, len + 1 + 8192); if (nb) { g_json = nb; g_cap = len + 1 + 8192; } }
    if (g_json && len + 1 <= g_cap) {       /* only publish a frame we actually stored */
        memcpy(g_json, json, len); g_json[len] = 0; g_len = len;
        g_seq++;
        pthread_cond_broadcast(&cv);        /* wake SSE pushers — a NEW frame really landed */
    }
    pthread_mutex_unlock(&lk);
}

static void refresh_stats(void)
{
    int fd = connect_daemon();
    if (fd < 0) return;
    const char *req = "GET /stats HTTP/1.0\r\nHost: trk\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return; }
    char buf[STATS_CAP]; int len = 0; ssize_t n;
    while (len < (int)sizeof(buf) - 1 && (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) len += (int)n;
    close(fd);
    buf[len < 0 ? 0 : len] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (body) { body += 4; pthread_mutex_lock(&lk); snprintf(g_tstats, sizeof g_tstats, "%s", body); pthread_mutex_unlock(&lk); }
}

/* one SSE session: send GET /stream, skip headers, extract `data: <json>` lines */
static void run_session(int fd)
{
    const char *req = "GET /stream HTTP/1.0\r\nHost: trk\r\nAccept: text/event-stream\r\n\r\n";
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
            g_connected = 1;
        }
        char *nl;
        while ((nl = memchr(acc, '\n', len))) {
            int ll = (int)(nl - acc);
            if (ll > 0 && acc[ll - 1] == '\r') acc[ll - 1] = 0; else acc[ll] = 0;
            if (!strncmp(acc, "data: ", 6)) store_msg(acc + 6, (int)strlen(acc + 6));
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
        if (fd < 0) { g_connected = 0; nap(1000); continue; }
        refresh_stats();
        run_session(fd);
        close(fd);
        g_connected = 0;
        pthread_mutex_lock(&lk); g_seq++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);  /* wake pushers so they emit disconnected */
        if (run_flag) nap(500);
    }
    return NULL;
}

static void *stats_poller(void *a)
{
    (void)a;
    while (run_flag) { if (g_connected) refresh_stats(); nap(700); }
    return NULL;
}

int trk_start(const char *host_port)
{
    if (host_port && *host_port) {
        char tmp[80]; snprintf(tmp, sizeof tmp, "%s", host_port);
        char *c = strchr(tmp, ':');
        if (c) { *c = 0; g_port = atoi(c + 1); }
        snprintf(g_host, sizeof g_host, "%s", tmp);
    }
    run_flag = 1;
    if (pthread_create(&th, NULL, reader, NULL) != 0) { run_flag = 0; return -1; }
    stats_th_ok = (pthread_create(&stats_th, NULL, stats_poller, NULL) == 0);
    fprintf(stderr, "trk: consuming EO tracker SSE at %s:%d/stream\n", g_host, g_port);
    return 0;
}

void trk_stop(void)
{
    if (run_flag) { run_flag = 0; pthread_join(th, NULL); if (stats_th_ok) pthread_join(stats_th, NULL); }
    free(g_json); g_json = NULL; g_cap = g_len = 0;
}

int trk_get_frame_json(char *buf, int cap)
{
    pthread_mutex_lock(&lk);
    int n = 0;
    if (g_json && g_len > 0 && g_len < cap) { memcpy(buf, g_json, g_len); buf[g_len] = 0; n = g_len; }
    pthread_mutex_unlock(&lk);
    return n;
}

int trk_connected(void) { return g_connected; }

int trk_wait_frame(unsigned *last_seq, char *buf, int cap, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&lk);
    while (run_flag && g_seq == *last_seq)
        if (pthread_cond_timedwait(&cv, &lk, &ts) == ETIMEDOUT) break;
    int n = 0;
    if (g_seq != *last_seq && g_connected && g_json && g_len > 0 && g_len < cap) {
        memcpy(buf, g_json, g_len); buf[g_len] = 0; n = g_len;
    }
    *last_seq = g_seq;
    pthread_mutex_unlock(&lk);
    return n;
}

void trk_ctl(const char *daemon_query)
{
    int fd = connect_daemon();
    if (fd < 0) return;
    char req[256];
    int n = snprintf(req, sizeof req, "GET /ctl?%s HTTP/1.0\r\nHost: trk\r\n\r\n", daemon_query);
    ssize_t w = write(fd, req, n); (void)w;
    close(fd);
    refresh_stats();
}

int trk_get_stats(char *buf, int cap)
{
    pthread_mutex_lock(&lk);
    int n = snprintf(buf, cap, "%s", g_tstats[0] ? g_tstats : "{}");
    pthread_mutex_unlock(&lk);
    return n;
}
