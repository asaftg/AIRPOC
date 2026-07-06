/* radar_movie.c — render the recorded radar data into a watchable scope movie.
 *
 * Radar isn't imagery — it's points + target boxes per frame. To get a "radar
 * movie" we draw a top-down PPI (radar at bottom-centre, range up): range rings,
 * FOV edges, detection points, and target boxes, one frame per recorded radar
 * frame, piped to ffmpeg -> H.264. Included in the offload tar as radar.mp4
 * alongside the EO native.mp4. This is a clean self-contained visualization of
 * the recorded data (not a copy of the GUI's scope).
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>

#define RW 720          /* scope image size */
#define RH 720
#define MARGIN 24

typedef struct { float x, y; } XY;

/* pull consecutive "x":/"y": pairs out of a JSON array slice */
static int parse_xy(const char *from, const char *to, XY *out, int max)
{
    int n = 0;
    const char *p = from;
    while (n < max && p < to) {
        const char *px = strstr(p, "\"x\":"); if (!px || px >= to) break;
        const char *py = strstr(px, "\"y\":"); if (!py || py >= to) break;
        out[n].x = (float)atof(px + 4);
        out[n].y = (float)atof(py + 4);
        n++;
        p = py + 4;
    }
    return n;
}

static void px_set(uint8_t *img, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= RW || y < 0 || y >= RH) return;
    uint8_t *p = img + ((size_t)y * RW + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}

static void fill_box(uint8_t *img, int cx, int cy, int hs, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = cy - hs; y <= cy + hs; y++)
        for (int x = cx - hs; x <= cx + hs; x++)
            if (abs(x - cx) == hs || abs(y - cy) == hs) px_set(img, x, y, r, g, b);  /* outline */
}

static void ring(uint8_t *img, int cx, int cy, int rad, uint8_t v)
{
    for (int a = 0; a < 3600; a++) {
        double t = a * M_PI / 1800.0;
        px_set(img, cx + (int)(rad * cos(t)), cy - (int)(rad * fabs(sin(t))), v, v, v);
    }
}

/* draw one radar frame's JSON into an rgb24 scope */
static void draw_frame(const char *json, uint8_t *img, double max_range)
{
    memset(img, 8, (size_t)RW * RH * 3);                 /* near-black */
    int cx = RW / 2, cy = RH - MARGIN;
    double ppm = (RH - 2 * MARGIN) / (max_range > 1 ? max_range : 500.0);

    /* range rings every 100 m + labels-as-brightness */
    for (int rm = 100; rm <= (int)max_range; rm += 100)
        ring(img, cx, cy, (int)(rm * ppm), 40);
    for (int y = MARGIN; y <= cy; y++) px_set(img, cx, y, 30, 30, 30);   /* boresight */

    /* detection points (green) */
    XY pts[2048];
    const char *ps = strstr(json, "\"points\":[");
    if (ps) {
        const char *pe = strchr(ps, ']');
        int n = parse_xy(ps, pe ? pe : json + strlen(json), pts, 2048);
        for (int i = 0; i < n; i++) {
            int x = cx + (int)(pts[i].x * ppm), y = cy - (int)(pts[i].y * ppm);
            px_set(img, x, y, 60, 230, 90);
            px_set(img, x+1, y, 60, 230, 90); px_set(img, x, y+1, 60, 230, 90);
        }
    }
    /* targets (red boxes) */
    XY tg[256];
    const char *ts = strstr(json, "\"targets\":[");
    if (ts) {
        const char *te = strchr(ts, ']');
        int n = parse_xy(ts, te ? te : json + strlen(json), tg, 256);
        for (int i = 0; i < n; i++)
            fill_box(img, cx + (int)(tg[i].x * ppm), cy - (int)(tg[i].y * ppm), 8, 240, 70, 70);
    }
}

static int build_radar_mp4(const char *sid)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);
    char ipath[700];
    snprintf(ipath, sizeof ipath, "%s/radar_wire/index.bin", dir);
    FILE *f = fopen(ipath, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    long n = sz / (long)sizeof(AirecIdxRow);
    if (n <= 0) { fclose(f); return -1; }
    AirecIdxRow *rows = malloc((size_t)sz);
    if (!rows || fread(rows, sizeof(AirecIdxRow), (size_t)n, f) != (size_t)n) { free(rows); fclose(f); return -1; }
    fclose(f);

    double fps = 26.0;
    if (n > 1) {
        double d = (rows[n-1].t_ns - rows[0].t_ns) / 1e9;
        if (d > 0) fps = (n-1)/d;
        if (fps < 1) fps = 1;
        if (fps > 60) fps = 60;
    }

    char tmp[720], cmd[1240];
    snprintf(tmp, sizeof tmp, "%s/radar.mp4.%lu.tmp", dir, (unsigned long)getpid());
    snprintf(cmd, sizeof cmd,
        "nice -n 15 ionice -c3 ffmpeg -hide_banner -loglevel error -y "
        "-f rawvideo -pix_fmt rgb24 -s %dx%d -r %.3f -i - "
        "-c:v libx264 -preset veryfast -crf 22 -pix_fmt yuv420p -movflags +faststart -f mp4 '%s'",
        RW, RH, fps, tmp);
    FILE *ff = popen(cmd, "w");
    if (!ff) { free(rows); return -1; }

    uint8_t *img = malloc((size_t)RW * RH * 3);
    char *json = malloc(256 * 1024);
    FILE *seg = NULL; uint32_t seg_no = 0xffffffff;
    int ok = img && json;
    for (long i = 0; i < n && ok; i++) {
        AirecIdxRow *r = &rows[i];
        if (r->segment_no != seg_no) {
            if (seg) fclose(seg);
            char sp[760]; snprintf(sp, sizeof sp, "%s/radar_wire/data.%05u.airec", dir, r->segment_no);
            seg = fopen(sp, "rb"); seg_no = r->segment_no;
            if (!seg) { ok = 0; break; }
        }
        uint32_t plen = r->payload_len < 256*1024-1 ? r->payload_len : 256*1024-1;
        if (fseek(seg, (long)r->offset + (long)sizeof(AirecRecHdr), SEEK_SET) != 0 ||
            fread(json, 1, plen, seg) != plen) continue;
        json[plen] = 0;
        char mr[24];
        double max_range = (store_manifest_field(json, "max_range_m", mr, sizeof mr) == 0 && atof(mr) > 1)
                         ? atof(mr) : 500.0;
        draw_frame(json, img, max_range);
        if (fwrite(img, 1, (size_t)RW * RH * 3, ff) != (size_t)RW * RH * 3) { ok = 0; break; }
    }
    if (seg) fclose(seg);
    free(img); free(json); free(rows);
    int rc = pclose(ff);

    char final[700];
    snprintf(final, sizeof final, "%s/radar.mp4", dir);
    if (ok && rc == 0 && rename(tmp, final) == 0) return 0;
    unlink(tmp);
    return -1;
}

int radar_movie_ensure(const char *sid)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return -1;
    char p[700];
    snprintf(p, sizeof p, "%s/%s/radar.mp4", g_rec.root, sid);
    if (access(p, F_OK) == 0) return 0;
    return build_radar_mp4(sid);
}
