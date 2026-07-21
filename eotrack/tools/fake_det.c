/* fake_det.c - a synthetic detector SSE server (bench only) so trackerd can be run
 * end to end without the camera/GPU. Serves GET /stream on a port, emitting ~15/s
 * det frames: one moving "human" (a translating box, promoted via tbd) plus a couple
 * of in-place "mover" blips that the tracker should latch off. Mirrors the real
 * detector wire shape closely enough to exercise det_feed's parser and the full
 * daemon path (http, tap, heartbeat). NOT shipped on the seeker. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

static uint64_t now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[512]; ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;
    const char *hdr = "HTTP/1.0 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
    if (write(fd, hdr, strlen(hdr)) < 0) { close(fd); return NULL; }
    uint64_t seq = 0;
    for (;;) {
        double t = seq / 15.0;
        double hx = 300 + 90.0 * t;          /* human moving right */
        if (hx > 1200) hx = 1200;
        uint64_t tp = now_ns();
        char buf[2048];
        int off = snprintf(buf, sizeof buf,
            "data: {\"type\":\"det\",\"frame_id\":%llu,\"t_src_ns\":%llu,"
            "\"t_pub_ns\":%llu,\"t_out_ns\":%llu,\"night\":false,"
            "\"illum\":{\"on\":false,\"present\":true,\"power\":0,\"fov10\":0},"
            "\"model\":\"fake\",\"img\":{\"w\":1440,\"h\":1088},\"ifov_urad\":287.5,"
            "\"tap_gaps\":0,\"drops_cum\":0,\"dets\":[",
            (unsigned long long)(seq * 4), (unsigned long long)(tp + 30000000000ull),
            (unsigned long long)tp, (unsigned long long)now_ns());
        off += snprintf(buf + off, sizeof buf - off,
            "{\"src\":\"app\",\"cls\":\"human\",\"hits\":5,\"tbd\":1,"
            "\"conf\":0.72,\"px\":[%.1f,540.0,40.0,80.0],\"ang\":[0,0,0,0]}", hx);
        off += snprintf(buf + off, sizeof buf - off, "],\"movers\":[");
        /* two in-place bouncing blips (class-less) -> should be latched off */
        off += snprintf(buf + off, sizeof buf - off,
            "{\"src\":\"mot\",\"age\":%d,\"conf\":0.4,\"px\":[%.1f,200.0,10.0,10.0],\"ang\":[0,0,0,0]},",
            (int)seq, 700.0 + ((seq % 2) ? 8.0 : -8.0));
        off += snprintf(buf + off, sizeof buf - off,
            "{\"src\":\"mot\",\"age\":%d,\"conf\":0.4,\"px\":[%.1f,850.0,9.0,9.0],\"ang\":[0,0,0,0]}]}",
            (int)seq, 400.0 + ((seq % 2) ? -6.0 : 6.0));
        if (dprintf(fd, "%s\n\n", buf) < 0) break;
        seq++;
        usleep(66000);   /* ~15 Hz */
    }
    close(fd); return NULL;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8094;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(port) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    listen(s, 8);
    fprintf(stderr, "fake_det on :%d\n", port);
    for (;;) { int fd = accept(s, NULL, NULL); if (fd < 0) continue;
        pthread_t t; pthread_create(&t, NULL, client, (void *)(long)fd); pthread_detach(t); }
    return 0;
}
