/* Radar SSE client — subscribes to the radar daemon's /stream (radar/ module, :8092)
 * and keeps the latest frame JSON. See radar.h. Reconnects forever; reports
 * connected=0 while the daemon is unreachable (the app must run fine with no radar).
 * The daemon and the app both run on the Jetson, so this is a localhost hop. */
#define _GNU_SOURCE
#include "radar.h"
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

#define ACC_CAP (128 * 1024)

static pthread_t       th;
static pthread_t       stats_th;
static int             stats_th_ok = 0;
/* Seconds a socket read may block before the session is abandoned. Bounds shutdown and
 * detects a wedged (open but silent) daemon; well above every feed's real message interval. */
#define RD_TIMEOUT_S 5

static volatile int    run_flag = 0;
/* The live session socket, so *_stop() can wake a thread parked in read() instead of waiting
 * out the timeout. -1 when no session is up. */
static volatile int    g_sess_fd = -1;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;   /* signalled on every new frame */
static char           *g_json = NULL;      /* latest frame JSON (grown as needed) */
static int             g_len = 0, g_cap = 0;
static unsigned        g_seq = 0;          /* bumps each stored frame (SSE push detector) */
static volatile int    g_connected = 0;    /* daemon reports a radar connected     */
static volatile int    g_ntargets = 0;
static char            g_rstats[512] = ""; /* daemon /stats JSON (controls + counts) */
static char            g_host[96] = "127.0.0.1";
static int             g_port = 8092;

/* Sleep in slices, so a shutdown is noticed within ~100 ms instead of at the end of a full
 * retry/poll interval. A stop that waits out every thread's longest sleep is a stop the
 * operator experiences as a hang. */
static void nap(int ms)
{
    while (ms > 0 && run_flag) {
        int s = ms > 100 ? 100 : ms;
        struct timespec t = { 0, (long)s * 1000000L };
        nanosleep(&t, NULL);
        ms -= s;
    }
}

static int connect_daemon(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    /* A READ TIMEOUT ON EVERY SOCKET. Without one, a session read blocks forever: the feed has
     * no reason to close a healthy connection, so the reader thread never looks at run_flag
     * again and the console could not be shut down at all — SIGTERM released the listen port
     * and then hung in pthread_join, leaving a process that still consumed every feed. It also
     * means a WEDGED daemon (socket open, nothing sent) ends the session and flips the console
     * to NOT CONNECTED instead of showing a frozen picture as live. Far above every real feed
     * rate, so it never recycles a working connection. */
    struct timeval tv = { RD_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
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
    if (g_json && len + 1 <= g_cap) {       /* only publish a frame we actually stored */
        memcpy(g_json, json, len); g_json[len] = 0; g_len = len;
        g_connected = conn; g_ntargets = nt;
        g_seq++;
        pthread_cond_broadcast(&cv);        /* wake SSE pushers — a NEW frame really landed */
    }
    pthread_mutex_unlock(&lk);
}

/* GET /stats and cache the daemon's raw JSON (fps/drops/counts + the current value of
 * all six controls), so the GUI can init + read back every slider against reality. */
static void refresh_stats(void)
{
    int fd = connect_daemon();
    if (fd < 0) return;
    const char *req = "GET /stats HTTP/1.0\r\nHost: radar\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return; }
    char buf[1024]; int len = 0; ssize_t n;
    while (len < (int)sizeof(buf) - 1 && (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) len += (int)n;
    close(fd);
    buf[len < 0 ? 0 : len] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (body) { body += 4; pthread_mutex_lock(&lk); snprintf(g_rstats, sizeof g_rstats, "%s", body); pthread_mutex_unlock(&lk); }
}

/* One-shot GET of the daemon's SCENE layer (radar/docs/SCENE_LAYER.md) — the static
 * occupancy backdrop, plus its own show/hide/clear controls carried in the same query. Not
 * cached and not part of the SSE session: the daemon only refreshes the snapshot once a
 * second, so the browser polls it at ~1 Hz through this pass-through. Payload is a few tens
 * of KB (lit cells only), so the caller owns a large buffer. Returns body length, or 0. */
int radar_get_scene(const char *query, char *buf, int cap)
{
    int fd = connect_daemon();
    if (fd < 0) return 0;
    char req[256];
    int rn = snprintf(req, sizeof req, "GET /scene%s%s HTTP/1.0\r\nHost: radar\r\n\r\n",
                      (query && *query) ? "?" : "", (query && *query) ? query : "");
    if (rn < 0 || rn >= (int)sizeof req || write(fd, req, (size_t)rn) < 0) { close(fd); return 0; }
    int len = 0; ssize_t n;
    while (len < cap - 1 && (n = read(fd, buf + len, (size_t)(cap - 1 - len))) > 0) len += (int)n;
    close(fd);
    if (len <= 0) return 0;
    buf[len] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    int blen = len - (int)(body - buf);
    memmove(buf, body, (size_t)blen);
    buf[blen] = 0;
    return blen;
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
        refresh_stats();                 /* sync control values on (re)connect */
        g_sess_fd = fd;
        run_session(fd);
        g_sess_fd = -1;
        close(fd);
        pthread_mutex_lock(&lk); g_connected = 0; g_ntargets = 0; pthread_mutex_unlock(&lk);
        if (run_flag) nap(500);
    }
    return NULL;
}

/* Poll the daemon's /stats a few times a second so the sliders reflect live (clamped)
 * values — the SSE frame thread blocks on /stream and can't also poll /stats. */
static void *stats_poller(void *a)
{
    (void)a;
    while (run_flag) { if (g_connected) refresh_stats(); nap(300); }
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
    stats_th_ok = (pthread_create(&stats_th, NULL, stats_poller, NULL) == 0);
    fprintf(stderr, "radar: consuming daemon SSE at %s:%d/stream\n", g_host, g_port);
    return 0;
}

void radar_stop(void)
{
    if (run_flag) {
        run_flag = 0;
        /* wake the reader NOW: it is parked in read() on a healthy socket the feed will never
         * close on its own, and join would otherwise wait out the read timeout (or, before that
         * timeout existed, forever) */
        int f = g_sess_fd; if (f >= 0) shutdown(f, SHUT_RDWR);
        pthread_join(th, NULL);
        if (stats_th_ok) pthread_join(stats_th, NULL);
    }
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

int radar_wait_frame(unsigned *last_seq, char *buf, int cap, int timeout_ms)
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
    if (g_seq != *last_seq && g_json && g_len > 0 && g_len < cap) {
        memcpy(buf, g_json, g_len); buf[g_len] = 0; n = g_len;
    }
    *last_seq = g_seq;
    pthread_mutex_unlock(&lk);
    return n;
}

/* Forward a raw control query (e.g. "eps=8&fov=60") to the daemon's /ctl, then re-read
 * /stats so the readback reflects the clamped result. Best-effort. */
void radar_ctl(const char *daemon_query)
{
    int fd = connect_daemon();
    if (fd < 0) return;
    char req[256];
    int n = snprintf(req, sizeof req, "GET /ctl?%s HTTP/1.0\r\nHost: radar\r\n\r\n", daemon_query);
    ssize_t w = write(fd, req, n); (void)w;
    close(fd);
    refresh_stats();
}

int radar_get_stats(char *buf, int cap)
{
    pthread_mutex_lock(&lk);
    int n = snprintf(buf, cap, "%s", g_rstats[0] ? g_rstats : "{}");
    pthread_mutex_unlock(&lk);
    return n;
}
