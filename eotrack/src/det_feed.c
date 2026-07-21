#define _GNU_SOURCE
#include "det_feed.h"
#include "config.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* ---------------- JSON scanning (tolerant, our own producer) ---------------- */

/* Find "key": within [s,end) at the current brace depth-agnostic level; returns a
 * pointer just past the colon, or NULL. Simple substring - safe because our keys
 * are unique within a box object and we scan per-object slices. */
static const char *val_of(const char *s, const char *end, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = s; p + kl + 2 < end; p++) {
        if (p[0] == '"' && !strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
            const char *q = p + 1 + kl + 1;
            while (q < end && (*q == ' ' || *q == ':')) q++;
            return q;
        }
    }
    return NULL;
}

static double num_of(const char *s, const char *end, const char *key, double dflt)
{
    const char *v = val_of(s, end, key);
    if (!v || v >= end) return dflt;
    return atof(v);
}

static uint64_t u64_of(const char *s, const char *end, const char *key)
{
    const char *v = val_of(s, end, key);
    if (!v || v >= end) return 0;
    return strtoull(v, NULL, 10);
}

/* Parse "px":[a,b,c,d] within [s,end). Returns 1 on success. */
static int arr4_of(const char *s, const char *end, const char *key, double o[4])
{
    const char *v = val_of(s, end, key);
    if (!v || v >= end || *v != '[') return 0;
    v++;
    for (int i = 0; i < 4; i++) {
        while (v < end && (*v == ' ' || *v == ',')) v++;
        if (v >= end) return 0;
        o[i] = atof(v);
        while (v < end && *v != ',' && *v != ']') v++;
    }
    return 1;
}

static int cls_code(const char *s, const char *end)
{
    const char *v = val_of(s, end, "cls");
    if (!v || v >= end || *v != '"') return 0;   /* absent (mover) -> unknown */
    v++;
    if (!strncmp(v, "human", 5))   return 1;
    if (!strncmp(v, "vehicle", 7)) return 2;
    if (!strncmp(v, "drone", 5))   return 3;
    return 0;
}

/* Scan one array ("dets" or "movers") appending boxes. src = 0 app / 1 mot. */
static int scan_array(const char *json, const char *key, int src,
                      TrkDet *out, int have, int max)
{
    const char *a = strstr(json, key);
    if (!a) return have;
    a = strchr(a, '[');
    if (!a) return have;
    /* find matching ] tracking nested [ ] */
    const char *p = a + 1; int depth = 1; const char *end = NULL;
    for (; *p; p++) { if (*p == '[') depth++; else if (*p == ']') { if (--depth == 0) { end = p; break; } } }
    if (!end) end = a + strlen(a);
    /* iterate objects: each begins at '{' and ends at the matching '}' */
    for (const char *o = a + 1; o < end && have < max; ) {
        if (*o != '{') { o++; continue; }
        const char *oe = o + 1; int od = 1;
        for (; oe < end; oe++) { if (*oe == '{') od++; else if (*oe == '}') { if (--od == 0) break; } }
        if (oe >= end) break;
        double px[4];
        if (arr4_of(o, oe, "px", px)) {
            TrkDet *d = &out[have++];
            d->src = src;
            d->cls = src == 1 ? 0 : cls_code(o, oe);
            d->conf = num_of(o, oe, "conf", 0.0);
            d->cx = px[0]; d->cy = px[1]; d->w = px[2]; d->h = px[3];
            d->tbd = (val_of(o, oe, "tbd") != NULL) ? 1 : 0;
            const char *hv = val_of(o, oe, "hits");
            d->hits = hv ? atoi(hv) : -1;
            const char *av = val_of(o, oe, "age");
            d->age = av ? atoi(av) : -1;
        }
        o = oe + 1;
    }
    return have;
}

int det_parse(const char *json, TrkDet *out, int max, DetMeta *meta)
{
    if (meta) {
        const char *end = json + strlen(json);
        meta->frame_id = u64_of(json, end, "frame_id");
        meta->t_src_ns = u64_of(json, end, "t_src_ns");
        meta->t_pub_ns = u64_of(json, end, "t_pub_ns");
        meta->t_out_ns = u64_of(json, end, "t_out_ns");
        const char *iv = val_of(json, end, "ifov_urad");
        if (iv) { meta->ifov_rad = atof(iv) * 1e-6; meta->have_ifov = 1; }
        else    { meta->have_ifov = 0; }
    }
    int n = 0;
    n = scan_array(json, "\"dets\"",   0, out, n, max);
    n = scan_array(json, "\"movers\"", 1, out, n, max);
    return n;
}

/* ------------------------- SSE consumer thread ------------------------------ */

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static struct {
    char       host[64];
    int        port;
    DetFrameCb cb;
    void      *user;
    uint64_t   last_frame_ns;
    pthread_mutex_t lk;
} g;

static int connect_det(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(g.port) };
    if (inet_pton(AF_INET, g.host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    const char *req = "GET /stream HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
    return fd;
}

static void handle_payload(const char *json)
{
    TrkDet dets[TRK_MAX_IN];
    DetMeta m; memset(&m, 0, sizeof m);
    int n = det_parse(json, dets, TRK_MAX_IN, &m);
    pthread_mutex_lock(&g.lk);
    g.last_frame_ns = now_ns();
    pthread_mutex_unlock(&g.lk);
    if (g.cb) g.cb(dets, n, &m, g.user);
}

static void *feed_thread(void *arg)
{
    (void)arg;
    static char buf[256 * 1024];
    int backoff_ms = 200;
    for (;;) {
        int fd = connect_det();
        if (fd < 0) { usleep(backoff_ms * 1000); if (backoff_ms < 2000) backoff_ms *= 2; continue; }
        backoff_ms = 200;
        size_t len = 0;
        /* skip response headers: read until "\r\n\r\n" */
        int hdr_done = 0;
        for (;;) {
            char tmp[16384];
            ssize_t r = read(fd, tmp, sizeof tmp);
            if (r <= 0) break;
            if (len + (size_t)r >= sizeof buf) len = 0;   /* overflow guard: drop */
            memcpy(buf + len, tmp, (size_t)r); len += (size_t)r; buf[len] = 0;
            if (!hdr_done) {
                char *h = strstr(buf, "\r\n\r\n");
                if (!h) continue;
                size_t off = (size_t)(h + 4 - buf);
                memmove(buf, buf + off, len - off); len -= off; buf[len] = 0;
                hdr_done = 1;
            }
            /* extract complete SSE events terminated by "\n\n" */
            char *ev;
            while ((ev = strstr(buf, "\n\n")) != NULL) {
                *ev = 0;
                char *d = strstr(buf, "data:");
                if (d) { d += 5; while (*d == ' ') d++; if (*d == '{') handle_payload(d); }
                size_t consumed = (size_t)(ev + 2 - buf);
                memmove(buf, buf + consumed, len - consumed + 1);
                len -= consumed;
            }
        }
        close(fd);
        usleep(backoff_ms * 1000);
    }
    return NULL;
}

int det_feed_start(const char *host, int port, DetFrameCb cb, void *user)
{
    memset(&g, 0, sizeof g);
    snprintf(g.host, sizeof g.host, "%s", host ? host : DET_STREAM_HOST);
    g.port = port; g.cb = cb; g.user = user;
    pthread_mutex_init(&g.lk, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, feed_thread, NULL)) return -1;
    pthread_detach(t);
    return 0;
}

int det_feed_connected(uint64_t stale_ns)
{
    pthread_mutex_lock(&g.lk);
    uint64_t last = g.last_frame_ns;
    pthread_mutex_unlock(&g.lk);
    if (!last) return 0;
    return (now_ns() - last) < stale_ns;
}
