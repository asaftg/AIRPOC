/* Operator GUI server. Two roles, both OFF the EO channel:
 *   (1) a rate-capped worker that reads the latest EO frame, shrinks+compresses it
 *       to one small JPEG per tick (encode once), and publishes it;
 *   (2) an HTTP server (one thread per connection) that fans that JPEG out to every
 *       screen and serves /stats + /ctl. No websockets.
 * The web page is embedded (web_assets.h). See app/docs/GUI.md. */
#define _GNU_SOURCE
#include "gui.h"
#include "eo_frame.h"
#include "radar.h"
#include "view.h"
#include "illum.h"
#include "web_assets.h"
#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <jpeglib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ state ---- */
static volatile sig_atomic_t g_run = 1;
static pthread_t g_enc_th, g_srv_th;
static int       g_listen_fd = -1;

/* stream controls (set via /ctl) */
static volatile int  g_zoom  = 1;         /* digital zoom 1/2/4/8              */
static volatile int  g_dw    = 512;       /* stream width; height from src aspect */
static volatile int  g_fps   = 60;        /* stream fps cap                    */
static volatile int  g_q     = 45;        /* JPEG quality                      */
static volatile int  g_track_man  = 0;    /* tracking: 0 auto / 1 manual       */
static volatile int  g_illum_auto = 1;    /* illuminator: 1 auto(fit+max)/0 man*/
static volatile int  g_engage     = -1;   /* engaged target tid, -1 = none     */
static char          g_preset[16] = "SMOOTH";
static pthread_mutex_t g_preset_lk = PTHREAD_MUTEX_INITIALIZER;

/* published picture + live stats */
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_jpeg = NULL;
static unsigned long   g_jlen = 0;
static uint64_t        g_jseq = 0;
static double          g_enc_fps = 0.0, g_src_fps = 0.0, g_mbps = 0.0;
static int             g_srcw = 1440, g_srch = 1088;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void set_preset(const char *name)
{
    int w = g_dw, f = g_fps, q = g_q;
    if      (!strncmp(name, "LEAN", 4))     { w = 400;  f = 60; q = 40; }
    else if (!strncmp(name, "SMOOTH", 6))   { w = 512;  f = 60; q = 45; }
    else if (!strncmp(name, "BALANCED", 8)) { w = 800;  f = 30; q = 60; }
    else if (!strncmp(name, "CLEAR", 5))    { w = 1152; f = 25; q = 75; }
    else return;
    g_dw = w; g_fps = f; g_q = q;
    pthread_mutex_lock(&g_preset_lk);
    snprintf(g_preset, sizeof(g_preset), "%.*s", (int)sizeof(g_preset) - 1, name);
    pthread_mutex_unlock(&g_preset_lk);
}

static double cam_hfov_deg(void)
{
    int z = g_zoom < 1 ? 1 : g_zoom;
    return 2 * atan((g_srcw * eo_pixel_um() / 1000.0 / z) / (2 * eo_focal_mm())) * 180.0 / M_PI;
}
static double cam_vfov_deg(void)
{
    int z = g_zoom < 1 ? 1 : g_zoom;
    return 2 * atan((g_srch * eo_pixel_um() / 1000.0 / z) / (2 * eo_focal_mm())) * 180.0 / M_PI;
}

/* SoC temperature (millidegrees C) from a thermal zone, or -1 if unreadable. */
static double read_temp_c(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1.0;
    long milli = -1;
    if (fscanf(f, "%ld", &milli) != 1) milli = -1;
    fclose(f);
    return milli < 0 ? -1.0 : milli / 1000.0;
}

/* ------------------------------------------------------------ jpeg encode ---- */
static unsigned char *encode_gray(const uint8_t *gray, int w, int h, int q,
                                  unsigned long *len)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr jerr;
    unsigned char *out = NULL;
    unsigned long sz = 0;
    c.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &sz);
    c.image_width = w; c.image_height = h;
    c.input_components = 1; c.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, q, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < (JDIMENSION)h) {
        JSAMPROW row = (JSAMPROW)(gray + (size_t)c.next_scanline * w);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    *len = sz;
    return out;
}

/* --------------------------------------------------------- encoder thread ---- */
static void *encoder(void *arg)
{
    (void)arg;
    uint8_t *sm = NULL;
    size_t   smcap = 0;
    uint64_t last_seq = 0;
    double   fps_ema = 0.0, t_prev = now_s();
    uint64_t src_prev_seq = 0;
    double   src_prev_t = now_s();

    while (g_run) {
        int fps = g_fps < 1 ? 1 : g_fps;
        double period = 1.0 / fps;
        double t0 = now_s();

        eo_frame_t f;
        if (eo_get_latest(&f) && f.seq != last_seq) {
            last_seq = f.seq;
            g_srcw = f.width; g_srch = f.height;

            int dw = g_dw; if (dw < 64) dw = 64; if (dw > f.width) dw = f.width;
            int dh = (int)((int64_t)dw * f.height / f.width); if (dh < 1) dh = 1;

            size_t need = (size_t)dw * dh;
            if (need > smcap) {
                uint8_t *tmp = realloc(sm, need);
                if (!tmp) { free(sm); sm = NULL; smcap = 0; continue; }
                sm = tmp; smcap = need;
            }
            view_shrink(&f, g_zoom, sm, dw, dh);

            unsigned long jl = 0;
            unsigned char *j = encode_gray(sm, dw, dh, g_q, &jl);
            if (j) {
                double t = now_s(), dt = t - t_prev; t_prev = t;
                if (dt > 0) {
                    double inst = 1.0 / dt;
                    fps_ema = fps_ema ? 0.9 * fps_ema + 0.1 * inst : inst;
                }
                pthread_mutex_lock(&g_lk);
                free(g_jpeg);
                g_jpeg = j; g_jlen = jl; g_jseq++;
                g_enc_fps = fps_ema;
                g_mbps = jl * fps_ema * 8.0 / 1e6;
                pthread_mutex_unlock(&g_lk);
            }
        }

        /* true source rate from seq deltas (independent of our poll), ~1 Hz */
        double tn = now_s();
        if (tn - src_prev_t >= 1.0) {
            eo_frame_t g;
            if (eo_get_latest(&g)) {
                double sf = (double)(g.seq - src_prev_seq) / (tn - src_prev_t);
                pthread_mutex_lock(&g_lk); g_src_fps = sf; pthread_mutex_unlock(&g_lk);
                src_prev_seq = g.seq;
            }
            src_prev_t = tn;
        }

        double spent = now_s() - t0, rem = period - spent;
        if (rem > 0) {
            struct timespec ts = { (time_t)rem, (long)((rem - (time_t)rem) * 1e9) };
            nanosleep(&ts, NULL);
        }
    }
    free(sm);
    return NULL;
}

/* --------------------------------------------------------------- http -------- */
static int has(const char *req, const char *path)   /* "GET <path>" prefix match */
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "GET %s", path);
    return strncmp(req, buf, (size_t)n) == 0;
}

static void send_asset(int fd, const char *ctype, const unsigned char *body, unsigned len)
{
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
                "Content-Length: %u\r\nConnection: close\r\n\r\n", ctype, len);
    ssize_t wr = write(fd, body, len); (void)wr;
}

/* Illuminator AUTO: fit the beam to the camera FOV at max power. */
static void apply_illum_auto(void)
{
    illum_set_fov(cam_hfov_deg());
    illum_set_power(255);
}

static void handle_ctl(const char *req)
{
    char *q;
    if ((q = strstr(req, "zoom=")))  { int v = atoi(q + 5); if (v==1||v==2||v==4||v==8) { g_zoom = v; if (g_illum_auto) apply_illum_auto(); } }
    if ((q = strstr(req, "res=")))   { int v = atoi(q + 4); if (v>=64 && v<=1600) g_dw = v; }
    if ((q = strstr(req, "fps=")))   { int v = atoi(q + 4); if (v>=1  && v<=60)   g_fps = v; }
    if ((q = strstr(req, "q=")))     { int v = atoi(q + 2); if (v>=10 && v<=95)   g_q  = v; }
    if ((q = strstr(req, "preset="))) {
        char nm[16] = {0};
        sscanf(q + 7, "%15[A-Z]", nm);
        set_preset(nm);
    }
    if ((q = strstr(req, "track=")))  g_track_man = (strncmp(q + 6, "man", 3) == 0);
    if ((q = strstr(req, "engage="))) g_engage = atoi(q + 7);
    if ((q = strstr(req, "illum=")))  { g_illum_auto = (strncmp(q + 6, "auto", 4) == 0); if (g_illum_auto) apply_illum_auto(); }
    if ((q = strstr(req, "laser=")))  illum_set_on(atoi(q + 6));
    if (!g_illum_auto) {                 /* manual beam control only when not auto */
        if ((q = strstr(req, "power=")))  illum_set_power(atoi(q + 6));
        if ((q = strstr(req, "fov=")))    illum_set_fov(atof(q + 4));
    }
}

static void handle_stats(int fd)
{
    pthread_mutex_lock(&g_lk);
    double efps = g_enc_fps, sfps = g_src_fps, mbps = g_mbps;
    int w = g_dw, h = (int)((int64_t)g_dw * g_srch / g_srcw);
    pthread_mutex_unlock(&g_lk);

    int lon, lpw, lpr; double lfov;
    illum_snapshot(&lon, &lpw, &lfov, &lpr);

    radar_frame_t rf;
    char tracks_s[16];
    if (radar_get_latest(&rf) && rf.connected) snprintf(tracks_s, sizeof tracks_s, "%d", rf.num_targets);
    else snprintf(tracks_s, sizeof tracks_s, "null");

    double cpu = read_temp_c("/sys/class/thermal/thermal_zone0/temp");
    char cpu_s[16]; if (cpu < 0) snprintf(cpu_s, sizeof cpu_s, "null");
    else snprintf(cpu_s, sizeof cpu_s, "%.0f", cpu);

    pthread_mutex_lock(&g_preset_lk);
    char preset[16]; snprintf(preset, sizeof preset, "%s", g_preset);
    pthread_mutex_unlock(&g_preset_lk);

    char body[720];
    int bl = snprintf(body, sizeof body,
        "{\"fps\":%.0f,\"src_fps\":%.0f,\"mbps\":%.2f,"
        "\"zoom\":%d,\"res_w\":%d,\"res_h\":%d,\"fps_cap\":%d,\"q\":%d,\"preset\":\"%s\","
        "\"hfov\":%.2f,\"vfov\":%.2f,\"track\":\"%s\",\"illum_mode\":\"%s\",\"engage\":%d,"
        "\"cpu_c\":%s,\"cam_c\":null,"
        "\"laser\":%d,\"lpower\":%d,\"lfov\":%.1f,\"lpresent\":%d,"
        "\"batt\":null,\"alt\":null,\"brg\":null,\"rng\":null,\"tracks\":%s}\n",
        efps, sfps, mbps,
        g_zoom, w, h, g_fps, g_q, preset,
        cam_hfov_deg(), cam_vfov_deg(), g_track_man ? "man" : "auto",
        g_illum_auto ? "auto" : "man", g_engage,
        cpu_s, lon, lpw, lfov, lpr, tracks_s);

    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s", bl, body);
}

static void handle_radar(int fd)
{
    radar_frame_t r;
    int ok = radar_get_latest(&r);
    size_t cap = 16384;
    char *b = malloc(cap);
    if (!b) { close(fd); return; }
    int n = snprintf(b, cap,
        "{\"connected\":%d,\"max_range_m\":%.0f,\"fov_half_deg\":%.0f,"
        "\"num_points\":%d,\"num_targets\":%d,\"points\":[",
        ok ? r.connected : 0, ok ? r.max_range_m : 0.0, ok ? r.fov_half_deg : 60.0,
        ok ? r.num_points : 0, ok ? r.num_targets : 0);
    if (ok) {
        for (int i = 0; i < r.num_points && n < (int)cap - 64; i++)
            n += snprintf(b + n, cap - n, "%s[%.1f,%.1f,%.1f,%.0f]",
                          i ? "," : "", r.points[i].x, r.points[i].y, r.points[i].v, r.points[i].snr);
        n += snprintf(b + n, cap - n, "],\"targets\":[");
        for (int i = 0; i < r.num_targets && n < (int)cap - 96; i++)
            n += snprintf(b + n, cap - n, "%s[%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f]",
                          i ? "," : "", r.targets[i].tid, r.targets[i].x, r.targets[i].y,
                          r.targets[i].vx, r.targets[i].vy, r.targets[i].sx, r.targets[i].sy,
                          r.targets[i].conf);
        n += snprintf(b + n, cap - n, "]}\n");
    } else {
        n += snprintf(b + n, cap - n, "],\"targets\":[]}\n");
    }
    dprintf(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s", n, b);
    free(b);
}

static void stream_mjpeg(int fd)
{
    const char *hdr = "HTTP/1.0 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (write(fd, hdr, strlen(hdr)) < 0) return;

    uint64_t last = 0;
    while (g_run) {
        pthread_mutex_lock(&g_lk);
        if (g_jseq == last || !g_jpeg) { pthread_mutex_unlock(&g_lk); usleep(2000); continue; }
        last = g_jseq;
        unsigned long len = g_jlen;
        unsigned char *copy = malloc(len);
        if (copy) memcpy(copy, g_jpeg, len);
        pthread_mutex_unlock(&g_lk);
        if (!copy) break;

        char part[128];
        int pl = snprintf(part, sizeof part,
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n", len);
        if (write(fd, part, pl) < 0 || write(fd, copy, len) < 0 || write(fd, "\r\n", 2) < 0) {
            free(copy); break;
        }
        free(copy);
    }
}

static void *client(void *arg)
{
    int fd = (int)(long)arg;
    char req[1024];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = 0;

    if (has(req, "/stats"))          handle_stats(fd);
    else if (has(req, "/radar"))     handle_radar(fd);
    else if (has(req, "/ctl")) {
        handle_ctl(req);
        const char *ok = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        ssize_t wr = write(fd, ok, strlen(ok)); (void)wr;
    }
    else if (has(req, "/stream"))    stream_mjpeg(fd);
    else if (has(req, "/app.css"))   send_asset(fd, "text/css", asset_app_css, asset_app_css_len);
    else if (has(req, "/app.js"))    send_asset(fd, "application/javascript", asset_app_js, asset_app_js_len);
    else                             send_asset(fd, "text/html", asset_index_html, asset_index_html_len);

    close(fd);
    return NULL;
}

static void *server(void *arg)
{
    int port = (int)(long)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("gui: bind"); return NULL; }
    listen(s, 16);
    g_listen_fd = s;

    while (g_run) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) { if (!g_run) break; continue; }
        pthread_t t;
        if (pthread_create(&t, NULL, client, (void *)(long)fd) == 0) pthread_detach(t);
        else close(fd);
    }
    close(s);
    return NULL;
}

int gui_start(int port)
{
    g_run = 1;
    if (pthread_create(&g_enc_th, NULL, encoder, NULL) != 0) return -1;
    if (pthread_create(&g_srv_th, NULL, server, (void *)(long)port) != 0) {
        g_run = 0; pthread_join(g_enc_th, NULL); return -1;
    }
    return 0;
}

void gui_stop(void)
{
    g_run = 0;
    if (g_listen_fd >= 0) { shutdown(g_listen_fd, SHUT_RDWR); }
    pthread_join(g_srv_th, NULL);
    pthread_join(g_enc_th, NULL);
    pthread_mutex_lock(&g_lk);
    free(g_jpeg); g_jpeg = NULL;
    pthread_mutex_unlock(&g_lk);
}
