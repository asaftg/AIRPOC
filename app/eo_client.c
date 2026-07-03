/* EO feed consumer — the operator console couples to the EO module's served feed
 * (eo/pipeline, MJPEG-over-HTTP on :8091), never its internals. The EO module owns
 * capture + AE + ISP + zoom + illuminator; this client subscribes to its MJPEG
 * /stream, keeps the latest JPEG frame, and mirrors its /stats. The app serves that
 * JPEG verbatim on /stream and forwards operator controls to the feed's /ctl. So the
 * app does ZERO capture/ISP/AE/encode. Reconnects forever; reports connected=0 while
 * the feed is down (the console shows NOT CONNECTED — no synthetic frames). */
#define _GNU_SOURCE
#include "eo_client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RDBUF (256 * 1024)

static pthread_t       th;
static pthread_t       stats_th;
static int             stats_th_ok = 0;
static volatile int    run_flag = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_jpeg = NULL;      /* latest JPEG frame (grown as needed) */
static int             g_len = 0, g_cap = 0;
static uint64_t        g_seq = 0;
static volatile int    g_connected = 0;
static char            g_stats[512] = "";  /* latest EO /stats JSON (for the console) */
static char            g_host[128] = "127.0.0.1";
static int             g_port = 8091;

static void nap(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static int connect_feed(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* Blocking read of exactly n bytes into dst. Returns 0 on success, -1 on EOF/err. */
static int read_full(int fd, unsigned char *dst, int n)
{
    int got = 0;
    while (got < n) {
        ssize_t r = read(fd, dst + got, n - got);
        if (r <= 0) return -1;
        got += (int)r;
    }
    return 0;
}

static void store_jpeg(const unsigned char *j, int len)
{
    pthread_mutex_lock(&lk);
    if (len > g_cap) { unsigned char *nb = realloc(g_jpeg, len + 4096); if (nb) { g_jpeg = nb; g_cap = len + 4096; } }
    if (g_jpeg && len <= g_cap) { memcpy(g_jpeg, j, len); g_len = len; g_seq++; }
    g_connected = 1;
    pthread_mutex_unlock(&lk);
}

/* Fetch the feed's /stats once (short-lived connection) and cache it. */
static void refresh_stats(void)
{
    int fd = connect_feed();
    if (fd < 0) return;
    const char *req = "GET /stats HTTP/1.0\r\nHost: eo\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return; }
    char buf[2048]; int len = 0; ssize_t r;
    while (len < (int)sizeof(buf) - 1 && (r = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) len += (int)r;
    close(fd);
    buf[len < 0 ? 0 : len] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        pthread_mutex_lock(&lk);
        snprintf(g_stats, sizeof g_stats, "%s", body);
        pthread_mutex_unlock(&lk);
    }
}

/* Read the MJPEG multipart stream: for each part, find Content-Length, read that many
 * JPEG bytes, store. Header/boundary lines are consumed line-by-line. */
static void run_stream(int fd)
{
    const char *req = "GET /stream HTTP/1.0\r\nHost: eo\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) return;
    unsigned char *buf = malloc(RDBUF);
    if (!buf) return;

    /* skip HTTP response headers up to the blank line */
    int hdr = 0, len = 0;
    while (hdr < 4 && len < RDBUF) {
        ssize_t r = read(fd, buf + len, 1);
        if (r <= 0) { free(buf); return; }
        char c = buf[len++];
        hdr = (c == '\r' || c == '\n') ? hdr + 1 : 0;
    }

    char line[256];
    for (;;) {
        /* read one header line of a part */
        int li = 0, clen = -1;
        int part_done = 0;
        while (!part_done) {
            li = 0;
            for (;;) {
                unsigned char c;
                if (read_full(fd, &c, 1) < 0) { free(buf); return; }
                if (c == '\n') break;
                if (c != '\r' && li < (int)sizeof(line) - 1) line[li++] = (char)c;
            }
            line[li] = 0;
            if (li == 0) { part_done = 1; break; }             /* blank line => body next */
            if (!strncasecmp(line, "Content-Length:", 15)) clen = atoi(line + 15);
        }
        if (clen <= 0 || clen > RDBUF) continue;
        if (read_full(fd, buf, clen) < 0) { free(buf); return; }
        store_jpeg(buf, clen);
    }
}

static void *reader(void *a)
{
    (void)a;
    while (run_flag) {
        int fd = connect_feed();
        if (fd < 0) { pthread_mutex_lock(&lk); g_connected = 0; pthread_mutex_unlock(&lk); nap(1000); continue; }
        refresh_stats();
        run_stream(fd);
        close(fd);
        pthread_mutex_lock(&lk); g_connected = 0; pthread_mutex_unlock(&lk);
        if (run_flag) nap(500);
    }
    return NULL;
}

/* /stats poller: run_stream() blocks on the MJPEG socket for the whole session, so the
 * stream thread can only snapshot /stats once at connect. This thread re-fetches /stats
 * a few times a second while frames flow, so zoom / laser / illuminator FOV+power the
 * operator changes are reflected back live (not frozen at connection time). */
static void *stats_poller(void *a)
{
    (void)a;
    while (run_flag) {
        if (g_connected) refresh_stats();
        nap(350);
    }
    return NULL;
}

int eo_start(const char *host_port)
{
    if (host_port && *host_port) {
        char tmp[100]; snprintf(tmp, sizeof tmp, "%s", host_port);
        char *c = strchr(tmp, ':');
        if (c) { *c = 0; g_port = atoi(c + 1); }
        snprintf(g_host, sizeof g_host, "%s", tmp);
    }
    run_flag = 1;
    if (pthread_create(&th, NULL, reader, NULL) != 0) { run_flag = 0; return -1; }
    stats_th_ok = (pthread_create(&stats_th, NULL, stats_poller, NULL) == 0);  /* non-fatal if it fails */
    fprintf(stderr, "eo: consuming EO feed at %s:%d/stream (proxy; no ISP here)\n", g_host, g_port);
    return 0;
}

void eo_stop(void)
{
    if (run_flag) { run_flag = 0; pthread_join(th, NULL); if (stats_th_ok) pthread_join(stats_th, NULL); }
    free(g_jpeg); g_jpeg = NULL; g_cap = g_len = 0;
}

int eo_get_jpeg(unsigned char *buf, int cap, uint64_t *seq)
{
    pthread_mutex_lock(&lk);
    int n = 0;
    if (g_jpeg && g_len > 0 && g_len <= cap) { memcpy(buf, g_jpeg, g_len); n = g_len; if (seq) *seq = g_seq; }
    pthread_mutex_unlock(&lk);
    return n;
}

int eo_connected(void) { return g_connected; }
int eo_last_len(void)  { return g_len; }

int eo_get_stats(char *buf, int cap)
{
    pthread_mutex_lock(&lk);
    int n = snprintf(buf, cap, "%s", g_stats);
    pthread_mutex_unlock(&lk);
    return n;
}

/* Forward an operator control to the EO feed's /ctl (zoom/laser/power/fov/ae/gain/...).
 * `query` is the raw query string, e.g. "zoom=4" or "laser=1". Best-effort. */
void eo_ctl(const char *query)
{
    int fd = connect_feed();
    if (fd < 0) return;
    char req[256];
    int n = snprintf(req, sizeof req, "GET /ctl?%s HTTP/1.0\r\nHost: eo\r\n\r\n", query);
    ssize_t w = write(fd, req, n); (void)w;
    close(fd);
}
