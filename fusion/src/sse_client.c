/* sse_client.c - see sse_client.h. Instance-based clone of the eotrack
 * det_feed consumer loop (same framing, timeouts and backoff). */
#define _GNU_SOURCE
#include "sse_client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

struct SseClient {
    char host[64];
    int  port;
    SsePayloadCb cb;
    SseDropCb    drop_cb;
    void *user;
    pthread_mutex_t lk;
    uint64_t last_frame_ns;
    int stopping;
    char buf[256 * 1024];
};

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int sse_connect(SseClient *c)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(c->port) };
    if (inet_pton(AF_INET, c->host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    const char *req = "GET /stream HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
    return fd;
}

static void *sse_thread(void *arg)
{
    SseClient *c = arg;
    int backoff_ms = 200;
    int was_connected = 0;
    while (!c->stopping) {
        int fd = sse_connect(c);
        if (fd < 0) {
            if (was_connected) { was_connected = 0; if (c->drop_cb) c->drop_cb(c->user); }
            usleep(backoff_ms * 1000);
            if (backoff_ms < 2000) backoff_ms *= 2;
            continue;
        }
        backoff_ms = 200;
        size_t len = 0;
        int hdr_done = 0;
        char *buf = c->buf;
        for (;;) {
            char tmp[16384];
            ssize_t r = read(fd, tmp, sizeof tmp);
            if (r <= 0) break;
            if (len + (size_t)r >= sizeof c->buf) len = 0;   /* overflow guard: drop */
            memcpy(buf + len, tmp, (size_t)r); len += (size_t)r; buf[len] = 0;
            if (!hdr_done) {
                char *h = strstr(buf, "\r\n\r\n");
                if (!h) continue;
                size_t off = (size_t)(h + 4 - buf);
                memmove(buf, buf + off, len - off); len -= off; buf[len] = 0;
                hdr_done = 1;
            }
            char *ev;
            while ((ev = strstr(buf, "\n\n")) != NULL) {
                *ev = 0;
                char *d = strstr(buf, "data:");
                if (d) {
                    d += 5; while (*d == ' ') d++;
                    if (*d == '{') {
                        pthread_mutex_lock(&c->lk);
                        c->last_frame_ns = now_ns();
                        pthread_mutex_unlock(&c->lk);
                        was_connected = 1;
                        if (c->cb) c->cb(d, c->user);
                    }
                }
                size_t consumed = (size_t)(ev + 2 - buf);
                memmove(buf, buf + consumed, len - consumed + 1);
                len -= consumed;
            }
        }
        close(fd);
        if (was_connected) { was_connected = 0; if (c->drop_cb) c->drop_cb(c->user); }
        usleep(backoff_ms * 1000);
    }
    free(c);
    return NULL;
}

SseClient *sse_client_start(const char *host, int port,
                            SsePayloadCb cb, SseDropCb drop_cb, void *user)
{
    SseClient *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->host, sizeof c->host, "%s", host ? host : "127.0.0.1");
    c->port = port; c->cb = cb; c->drop_cb = drop_cb; c->user = user;
    pthread_mutex_init(&c->lk, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, sse_thread, c)) { free(c); return NULL; }
    pthread_detach(t);
    return c;
}

int sse_client_connected(const SseClient *c, uint64_t stale_ns)
{
    SseClient *m = (SseClient *)c;
    pthread_mutex_lock(&m->lk);
    uint64_t last = m->last_frame_ns;
    pthread_mutex_unlock(&m->lk);
    if (!last) return 0;
    return (now_ns() - last) < stale_ns;
}

void sse_client_stop(SseClient *c){ if (c) c->stopping = 1; }
