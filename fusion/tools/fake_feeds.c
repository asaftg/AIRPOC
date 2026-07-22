/* fake_feeds.c - loopback SSE servers for the smoke test (tier 3). Serves a
 * small scripted scene as the radar wire on one port and the EO tracker wire
 * on another, at their real rates. DEV TOOL ONLY - never run in front of an
 * operator (no-sim rule); the smoke test uses non-production ports.
 *
 * usage: fake_feeds [rad_port] [trk_port]   (defaults 18092 18095)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define D2R (M_PI / 180.0)

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* one target closing from 200 m at 6 m/s, az 1 deg, el 0.5 deg */
static void scene(double t, double *az, double *el, double *r, double *rdot)
{
    *az = 1.0 * D2R; *el = 0.5 * D2R;
    *r = 200 - 6 * t; if (*r < 50) *r = 50;
    *rdot = *r > 50 ? -6 : 0;
}

static void *serve(void *arg)
{
    int is_rad = ((long)arg) >> 16;
    int port = ((long)arg) & 0xffff;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("fake_feeds bind"); exit(1); }
    listen(s, 4);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        char req[512]; ssize_t rr = read(fd, req, sizeof req - 1); (void)rr;
        const char *hdr = "HTTP/1.0 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
        if (write(fd, hdr, strlen(hdr)) < 0) { close(fd); continue; }
        double t0 = (double)now_ns() / 1e9;
        uint64_t fid = 0;
        for (;;) {
            double t = (double)now_ns() / 1e9 - t0;
            double az, el, r, rdot;
            scene(t, &az, &el, &r, &rdot);
            char msg[2048]; int n;
            if (is_rad) {
                double x = r * cos(el) * sin(az), y = r * cos(el) * cos(az), z = r * sin(el);
                double vx = rdot * sin(az), vy = rdot * cos(az);
                n = snprintf(msg, sizeof msg,
                    "data: {\"connected\":true,\"frame_id\":%llu,\"timestamp\":%.3f,"
                    "\"profile\":\"fake\",\"max_range_m\":500.0,\"fov_half_deg\":60.0,"
                    "\"num_points\":0,\"num_targets\":1,\"points\":[],\"targets\":["
                    "{\"tid\":1,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
                    "\"vx\":%.3f,\"vy\":%.3f,\"vz\":0.000,"
                    "\"sx\":1.000,\"sy\":1.000,\"sz\":0.500,"
                    "\"conf\":1.000,\"np\":40,\"sus\":0,\"mv_class\":1,"
                    "\"class\":\"radar_detection\"}]}\n\n",
                    (unsigned long long)++fid, (double)now_ns() / 1e9,
                    x, y, z, vx, vy);
            } else {
                n = snprintf(msg, sizeof msg,
                    "data: {\"type\":\"trk\",\"connected\":true,\"mode\":\"stare\","
                    "\"engaged\":-1,\"frame_id\":%llu,\"t_src_ns\":0,"
                    "\"t_pub_ns\":%llu,\"t_out_ns\":%llu,"
                    "\"img\":{\"w\":1440,\"h\":1088},\"ifov_urad\":287.5,\"tracks\":["
                    "{\"tid\":7,\"state\":\"conf\",\"cls\":\"vehicle\",\"cls_conf\":0.90,"
                    "\"conf\":0.800,\"px\":[720.0,540.0,40.0,30.0],"
                    "\"ang\":[%.4f,%.4f,0.0100,0.0080],\"rate\":[0.0000,0.0000],"
                    "\"s_ang\":[0.0010,0.0010],\"grow\":%.3f,"
                    "\"hits\":50,\"age_s\":4.00,\"coast_s\":0.00,"
                    "\"t_meas_ns\":0,\"src\":\"app\"}]}\n\n",
                    (unsigned long long)++fid,
                    (unsigned long long)now_ns(), (unsigned long long)now_ns(),
                    az, el, rdot < 0 ? -rdot / r : 0.0);
            }
            if (write(fd, msg, (size_t)n) < 0) break;
            usleep(is_rad ? 38461 : 66666);   /* 26 Hz / 15 Hz */
        }
        close(fd);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int rad_port = argc > 1 ? atoi(argv[1]) : 18092;
    int trk_port = argc > 2 ? atoi(argv[2]) : 18095;
    pthread_t a, b;
    pthread_create(&a, NULL, serve, (void *)((1L << 16) | rad_port));
    pthread_create(&b, NULL, serve, (void *)(long)trk_port);
    fprintf(stderr, "fake_feeds: radar :%d, tracker :%d\n", rad_port, trk_port);
    pthread_join(a, NULL);
    return 0;
}
