/* events.c — metadata capture: 5 Hz poll of the modules' documented /stats
 * surfaces into the events channel. Consumes contracts only, never internals.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* Minimal HTTP/1.0 GET against 127.0.0.1:<port>; returns body length or -1. */
int http_get_local(int port, const char *path, char *out, size_t outlen)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }

    char req[256];
    int rn = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: l\r\n\r\n", path);
    if (write(fd, req, (size_t)rn) != rn) { close(fd); return -1; }

    char resp[65536];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof resp - 1 && (r = read(fd, resp + n, sizeof resp - 1 - n)) > 0)
        n += (size_t)r;
    close(fd);
    resp[n] = 0;

    char *body = strstr(resp, "\r\n\r\n");
    if (!body) return -1;
    body += 4;
    size_t blen = n - (size_t)(body - resp);
    if (blen >= outlen) blen = outlen - 1;
    memcpy(out, body, blen);
    out[blen] = 0;
    /* strip trailing whitespace so it embeds cleanly in event JSON */
    while (blen && (out[blen - 1] == '\n' || out[blen - 1] == '\r' || out[blen - 1] == ' '))
        out[--blen] = 0;
    return (int)blen;
}

static void poll_one(const char *type, int port)
{
    char body[16384], ev[20480];
    if (http_get_local(port, "/stats", body, sizeof body) <= 0) return;
    if (body[0] != '{') return;
    int n = snprintf(ev, sizeof ev, "{\"type\":\"%s\",\"t_mono_ns\":%llu,\"body\":%s}",
                     type, (unsigned long long)now_ns(), body);
    if (n > 0 && (size_t)n < sizeof ev)
        chan_submit(CH_EVENTS, ev, (uint32_t)n, now_ns(), NULL);
}

static void *events_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 200000000 };            /* 5 Hz */
        nanosleep(&ts, NULL);
        if (!chan_recording()) continue;
        poll_one("eo_stats", REC_EO_PORT);
        poll_one("radar_stats", REC_RD_PORT);
        poll_one("app_stats", REC_APP_PORT);
    }
    return NULL;
}

void events_start(void)
{
    static pthread_t t;
    pthread_create(&t, NULL, events_thread, NULL);
}
