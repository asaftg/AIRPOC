/* AIRPOC radar previewer daemon.
 *
 *   ./radar_preview [-C /dev/radar-cli] [-D /dev/radar-data] [-c cfg]
 *                   [-b 3125000] [-p 8092] [-w web] [-n]
 *
 * Pushes the A/G profile over the CLI UART, then reads the data UART and
 * for every complete frame: parse (drop-free) -> cluster -> publish a
 * snapshot for the SSE previewer on :8092. The UART->frame pipeline runs
 * in this thread and never blocks on HTTP clients, so display can lag but
 * frames are never dropped. Missing ports degrade gracefully (retry).
 *
 * All detections are class-less — person/vehicle labelling is fusion's. */
#define _GNU_SOURCE
#include "radar.h"
#include "serial.h"
#include "cfg_push.h"
#include "tlv.h"
#include "cluster.h"
#include "wire.h"
#include "http.h"
#include "sim.h"
#include "airpoc_tap.h"   /* vendored from recorder/tap (protocol v1) — recorder taps */
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JSON_CAP (256 * 1024)
#define AG_MAX_RANGE_M   500.0
/* cfg publishes the full angular span (aoaFovCfg ±90); the GUI slider trims
 * azimuth live and defaults to the ~±60 useful-AoA region. */
#define AG_FOV_HALF_DEG   90.0

static volatile int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* /ctl handler — pushes live tracker changes to the clusterer. */
static void on_ctl(double eps_m, int min_pts, double speed_min, double snr_min,
                   double fov_half, double el_max, double doppler, int confirm,
                   double coast_s, double park_s, void *user) {
    cluster_set_dbscan((RadarClusterer *)user, eps_m, min_pts);
    cluster_set_gates((RadarClusterer *)user, speed_min, snr_min, fov_half, el_max, doppler);
    cluster_set_track((RadarClusterer *)user, confirm, coast_s, park_s);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    RadarClusterer *clust;
    RadarFrame      frame;
    char           *json;
    const char     *profile;
    double          fps;
    double          last_t;
    uint32_t        last_frame_no;
    int             have_last;
    int             warmup;      /* skip drop accounting for the first few
                                    frames after (re)connect — the initial
                                    frameNumber discontinuity is the connect
                                    gap, not a real dropped frame */
    unsigned long   drops;
    AirTap          raw_tap;     /* airpoc.radar_raw — every UART read(), pre-parse */
    AirTap          wire_tap;    /* airpoc.radar_wire — SSE frame JSON, verbatim */
    AirTap          cli_tap;     /* airpoc.radar_cli — chip CLI telemetry, 1 Hz */
} Ctx;

/* Called once per complete radar frame by the parser. */
static void on_frame(void *user, uint32_t frame_no, const RadarPoint *pts, int n,
                     const RadarStats *stats) {
    Ctx *c = user;
    double t = now_s();
    double dt = c->last_t > 0 ? (t - c->last_t) : 0.05;
    c->last_t = t;

    /* Drop accounting from frameNumber gaps (chip counts monotonically).
     * Skip while warming up so the (re)connect gap isn't counted as drops. */
    if (c->have_last && c->warmup == 0 && frame_no > c->last_frame_no + 1)
        c->drops += (frame_no - c->last_frame_no - 1);
    if (c->warmup > 0) c->warmup--;
    c->last_frame_no = frame_no;
    c->have_last = 1;

    if (n > RADAR_MAX_POINTS) n = RADAR_MAX_POINTS;
    memcpy(c->frame.points, pts, (size_t)n * sizeof(RadarPoint));
    c->frame.n_points = n;
    c->frame.frame_number = frame_no;

    int nt = cluster_step(c->clust, c->frame.points, n, t, dt,
                          c->frame.targets, RADAR_MAX_TARGETS);
    c->frame.n_targets = nt;

    if (dt > 0) {
        double inst = 1.0 / dt;
        c->fps = c->fps > 0 ? 0.85 * c->fps + 0.15 * inst : inst;
    }

    double fov = cluster_fov(c->clust);   /* live azimuth gate → wedge */
    int len = wire_frame_json(c->json, JSON_CAP, &c->frame, t,
                              AG_MAX_RANGE_M, fov, c->profile);
    /* Recorder tap (WI-RD-2): byte-verbatim SSE JSON. No-op if the tap failed
     * to create. meta = {frameNumber, n_points, n_targets, 0,0,0}. */
    uint32_t wire_meta[TAP_META_WORDS] = {
        frame_no, (uint32_t)c->frame.n_points, (uint32_t)c->frame.n_targets, 0, 0, 0 };
    tap_write(&c->wire_tap, c->json, (uint32_t)len, tap_now_ns(), wire_meta);
    http_publish(c->json, (size_t)len);
    http_set_stats(c->fps, c->drops, c->frame.n_points, c->frame.n_targets,
                   1, c->profile, AG_MAX_RANGE_M, fov);
    if (stats)
        http_set_timing(stats->interframe_proc_us, stats->interframe_margin_us,
                        stats->active_cpu_pct, stats->interframe_cpu_pct);
}

/* Resolve `rel` against the directory of THIS executable, so the default cfg and
 * web paths work no matter what CWD the daemon is launched from (the binary lives
 * in radar/src/, its cfg in ../cfg, its web page in ../web). Returns a malloc'd
 * absolute path, or a copy of `rel` if /proc/self/exe can't be read. */
static char *exe_relative(const char *rel) {
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) return strdup(rel);
    exe[k] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return strdup(rel);
    *slash = 0;                                  /* exe -> directory of the binary */
    size_t n = strlen(exe) + 1 + strlen(rel) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s/%s", exe, rel);
    return out ? out : strdup(rel);
}

int main(int argc, char **argv) {
    /* Device defaults are the udev symlinks (radar/udev/70-radar.rules creates
     * them, stable across ACM renumbering). Without that rule, pass the raw
     * nodes: -C /dev/ttyACM0 (XDS110 if00, CLI) -D /dev/ttyACM1 (if03, data). */
    const char *cli_dev  = "/dev/radar-cli";
    const char *data_dev = "/dev/radar-data";
    const char *cfg      = NULL;   /* NULL => exe-relative ../cfg/awr2944P_ag.cfg */
    const char *webroot  = NULL;   /* NULL => exe-relative ../web                 */
    int http_port = 8092, data_baud = 3125000, cli_baud = 115200, skip_cfg = 0, sim = 0, opt;

    while ((opt = getopt(argc, argv, "C:D:c:b:p:w:ns")) != -1) {
        switch (opt) {
            case 'C': cli_dev = optarg; break;
            case 'D': data_dev = optarg; break;
            case 'c': cfg = optarg; break;
            case 'b': data_baud = atoi(optarg); break;
            case 'p': http_port = atoi(optarg); break;
            case 'w': webroot = optarg; break;
            case 'n': skip_cfg = 1; break;
            case 's': sim = 1; break;
            default:
                fprintf(stderr, "usage: %s [-C cli] [-D data] [-c cfg] "
                        "[-b baud] [-p port] [-w web] [-n] [-s]\n", argv[0]);
                return 2;
        }
    }

    /* Resolve default cfg/web relative to the binary so `./radar_preview` works
     * from any directory (a common bring-up mistake was running from radar/src,
     * where the CWD-relative "cfg/…" and "web" don't exist). An explicit -c/-w
     * is honoured verbatim. */
    char *cfg_owned = NULL, *web_owned = NULL;
    if (!cfg)     cfg     = cfg_owned = exe_relative("../cfg/awr2944P_ag.cfg");
    if (!webroot) webroot = web_owned = exe_relative("../web");

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);      /* dropped SSE clients must not kill us */

    /* Profile label = cfg basename. */
    const char *base = strrchr(cfg, '/');
    const char *profile = base ? base + 1 : cfg;

    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.clust = cluster_new();
    ctx.json  = malloc(JSON_CAP);
    ctx.profile = profile;
    if (!ctx.clust || !ctx.json) { perror("malloc"); return 1; }

    http_start(http_port, webroot);
    http_set_ctl_cb(on_ctl, ctx.clust);      /* GET /ctl?eps=&minpts= -> DBSCAN */
    fprintf(stderr, "radar_preview: previewer http://0.0.0.0:%d/  profile=%s\n",
            http_port, profile);

    /* Simulation mode: no hardware. Feed synthetic mmw_demo TLV bytes
     * through the real parser+clusterer so the whole pipeline and the
     * previewer run with the board off (for the GUI agent to develop). */
    if (sim) {
        fprintf(stderr, "radar_preview: SIMULATION mode (no board)\n");
        TLVStream *st = tlv_stream_new();
        if (!st) { perror("tlv_stream_new"); return 1; }
        static uint8_t sbuf[64 * 1024];
        uint32_t fn = 0;
        double t0 = now_s();
        while (g_run) {
            size_t n = sim_build_frame(sbuf, sizeof(sbuf), fn++, now_s() - t0);
            if (n) tlv_stream_feed(st, sbuf, n, on_frame, &ctx);
            usleep(50000);      /* 20 Hz */
        }
        tlv_stream_free(st);
        cluster_free(ctx.clust); free(ctx.json);
        free(cfg_owned); free(web_owned);
        return 0;
    }

    TLVStream *stream = tlv_stream_new();
    if (!stream) { perror("tlv_stream_new"); return 1; }

    /* Recorder taps (WI-RD-1/2). Create once; failure logs once and leaves the
     * handle a no-op — a recorder-less system runs unchanged. radar_raw: every
     * UART read (512 slots x 8 KiB); radar_wire: SSE JSON (16 slots x 256 KiB). */
    if (tap_create(&ctx.raw_tap, "airpoc.radar_raw", 512, 8192,
                   "{\"name\":\"radar_raw\"}") < 0)
        fprintf(stderr, "radar_preview: radar_raw tap unavailable — recording disabled for it\n");
    if (tap_create(&ctx.wire_tap, "airpoc.radar_wire", 16, 256 * 1024,
                   "{\"name\":\"radar_wire\"}") < 0)
        fprintf(stderr, "radar_preview: radar_wire tap unavailable — recording disabled for it\n");
    /* radar_cli: the chip's own CLI telemetry (queryDemoStatus), polled at 1 Hz.
     * This data exists ONLY on the CLI UART — it is not in the TLV stream — so
     * without this tap it is unrecoverable after a recording is made, and every
     * question about it needs someone live at the bench. Carries the empty-band
     * comb-gate margin histogram (how we separate real targets from DDM comb
     * artifacts), sensor state, UART deferred-frame count, RF calibration status
     * and chip temperature. Cheap: ~1 KB/s. */
    if (tap_create(&ctx.cli_tap, "airpoc.radar_cli", 64, 8192,
                   "{\"name\":\"radar_cli\"}") < 0)
        fprintf(stderr, "radar_preview: radar_cli tap unavailable — telemetry not recorded\n");

    /* We push the cfg AT MOST ONCE, and only if the chip is silent. Re-pushing
     * against a live chip sends `sensorStop`, and this firmware won't restart
     * the sensor without a power-cycle — so a blind re-push on daemon restart
     * kills the stream. `-n` forces "already configured" (never push). */
    int cfg_settled = skip_cfg;

    uint8_t buf[8192];
    while (g_run) {
        int fd = serial_open(data_dev, data_baud);
        if (fd < 0) {
            http_set_stats(0, ctx.drops, 0, 0, 0, profile, AG_MAX_RANGE_M, AG_FOV_HALF_DEG);
            sleep(1);
            continue;
        }
        double last_rx = now_s();
        ctx.warmup = 5;   /* ignore the connect-gap frameNumber jump */

        if (!cfg_settled) {
            /* Read-first: peek ~2.5 s. Data present ⇒ chip already streaming ⇒
             * do NOT push. Silent ⇒ fresh boot ⇒ push the cfg over the CLI. */
            int saw = 0;
            double t_end = now_s() + 2.5;
            while (g_run && now_s() < t_end) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) { tap_write(&ctx.raw_tap, buf, (uint32_t)n, tap_now_ns(), NULL);
                             tlv_stream_feed(stream, buf, (size_t)n, on_frame, &ctx);
                             saw = 1; last_rx = now_s(); break; }
                if (n < 0) break;
            }
            if (saw) {
                fprintf(stderr, "radar_preview: chip already streaming — not pushing cfg\n");
            } else {
                fprintf(stderr, "radar_preview: port silent — pushing cfg over CLI\n");
                /* Wait for the CLI node: the board can enumerate a few seconds
                 * after boot, and both ACM ports may appear late. Retry ~10 s
                 * before giving up rather than skipping the push outright. */
                int cli = -1;
                for (int a = 0; a < 20 && g_run; a++) {
                    cli = serial_open(cli_dev, cli_baud);
                    if (cli >= 0) break;
                    if (a == 0)
                        fprintf(stderr, "radar_preview: CLI %s not ready — waiting…\n", cli_dev);
                    usleep(500 * 1000);
                }
                if (cli >= 0) {
                    if (cfg_push(cli, cfg) < 0)
                        fprintf(stderr, "radar_preview: cfg push had errors — continuing\n");
                    close(cli);
                } else {
                    fprintf(stderr, "radar_preview: CLI %s unavailable after retries — "
                            "chip may be unconfigured (pass -C for the right node)\n", cli_dev);
                }
            }
            cfg_settled = 1;
        }
        fprintf(stderr, "radar_preview: data %s @ %d baud — streaming\n", data_dev, data_baud);
        /* CLI telemetry poller. cfg_push (above) closed its own fd, so the CLI
         * UART is free while streaming. Fully non-blocking: write the query, then
         * collect the reply across later loop iterations, so frame reading is
         * never stalled waiting on the chip. */
        int cli_fd = serial_open(cli_dev, cli_baud);
        if (cli_fd < 0)
            fprintf(stderr, "radar_preview: CLI %s unavailable — telemetry not recorded\n", cli_dev);
        static uint8_t clibuf[8192];
        size_t clilen = 0;
        double cli_last = 0.0, cli_sent = 0.0;
        while (g_run) {
            if (cli_fd >= 0) {
                if (cli_sent > 0.0) {                     /* collecting a reply */
                    ssize_t r = read(cli_fd, clibuf + clilen, sizeof(clibuf) - clilen - 1);
                    if (r > 0) clilen += (size_t)r;
                    clibuf[clilen] = 0;
                    if ((clilen > 0 && strstr((char *)clibuf, "Done") != NULL)
                        || now_s() - cli_sent > 1.5) {
                        if (clilen > 0)
                            tap_write(&ctx.cli_tap, clibuf, (uint32_t)clilen, tap_now_ns(), NULL);
                        clilen = 0; cli_sent = 0.0;
                    }
                } else if (now_s() - cli_last >= 1.0) {   /* time to ask again */
                    cli_last = now_s();
                    const char *q = "queryDemoStatus\n";
                    if (write(cli_fd, q, strlen(q)) > 0) cli_sent = now_s();
                }
            }
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                /* Recorder tap (WI-RD-1): raw bytes BEFORE the parser, so capture
                 * is independent of parse health. No-op if the tap is disabled. */
                tap_write(&ctx.raw_tap, buf, (uint32_t)n, tap_now_ns(), NULL);
                tlv_stream_feed(stream, buf, (size_t)n, on_frame, &ctx);
                last_rx = now_s();
            } else if (n < 0) {
                break;                       /* hard error: reopen */
            } else if (now_s() - last_rx > 3.0) {
                fprintf(stderr, "radar_preview: no data 3 s — reopening\n");
                http_set_stats(0, ctx.drops, 0, 0, 0, profile, AG_MAX_RANGE_M, AG_FOV_HALF_DEG);
                break;
            }
        }
        close(fd);
        if (cli_fd >= 0) close(cli_fd);
        ctx.have_last = 0;                   /* reset drop tracking across reconnects */
    }

    fprintf(stderr, "radar_preview: shutting down\n");
    tap_destroy(&ctx.raw_tap);
    tap_destroy(&ctx.wire_tap);
    tap_destroy(&ctx.cli_tap);
    tlv_stream_free(stream);
    cluster_free(ctx.clust);
    free(ctx.json);
    free(cfg_owned); free(web_owned);
    return 0;
}
