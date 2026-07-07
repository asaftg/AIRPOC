/* transcode.c — build a smooth, seekable H.264 MP4 of a session's NATIVE replay.
 *
 * Native full-res JPEG-per-frame is too heavy to play smoothly over WiFi. So on
 * replay open we transcode the raw Y10 (through the exact tone map) into one
 * H.264 file the browser buffers and plays natively — full quality, smooth,
 * instant seek, and ~10-30x smaller on the wire. Encoded ONCE, cached in the
 * session dir. Runs niced below the live pipeline so it can never touch realtime.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include "eo_tonemap.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

typedef struct {
    char sid[SID_LEN + 1];
    int  state;                 /* 2 ready, 1 building, 0 none, -1 failed */
    int  pct;
    pthread_t tid;
} Job;

static Job g_job;               /* one at a time (one replay open at a time) */
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;

int transcode_mp4_path(const char *sid, char *path, size_t plen)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return -1;
    snprintf(path, plen, "%s/%s/native.mp4", g_rec.root, sid);
    return access(path, F_OK) == 0 ? 0 : -1;
}

int transcode_status(const char *sid, int *pct)
{
    char p[640];
    if (transcode_mp4_path(sid, p, sizeof p) == 0) { if (pct) *pct = 100; return 2; }
    pthread_mutex_lock(&g_lk);
    int st = (!strcmp(g_job.sid, sid)) ? g_job.state : 0;
    if (pct) *pct = (!strcmp(g_job.sid, sid)) ? g_job.pct : 0;
    pthread_mutex_unlock(&g_lk);
    return st;
}

/* read one channel's index rows + a payload fetcher, straight off disk */
static AirecIdxRow *load_idx(const char *dir, const char *chan, long *n)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s/index.bin", dir, chan);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    *n = sz / (long)sizeof(AirecIdxRow);
    AirecIdxRow *rows = malloc((size_t)sz);
    if (!rows || fread(rows, sizeof(AirecIdxRow), (size_t)*n, f) != (size_t)*n) { free(rows); rows = NULL; }
    fclose(f);
    return rows;
}

/* Build native.mp4 for a session (blocking). pct (optional) tracks progress.
 * Unique tmp per caller so a replay-triggered build and an export build can't
 * clobber each other. Returns 0 on success. */
static int build_mp4(const char *sid, volatile int *pct)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);

    int w = 1440, h = 1088, mode = MODE_Y10P;
    char cj[512], v[24], cjp[700];
    snprintf(cjp, sizeof cjp, "%s/eo_y10/channel.json", dir);
    FILE *cf = fopen(cjp, "rb");
    if (cf) {
        size_t nn = fread(cj, 1, sizeof cj - 1, cf); cj[nn] = 0; fclose(cf);
        if (store_manifest_field(cj, "w", v, sizeof v) == 0 && atoi(v) > 0) w = atoi(v);
        if (store_manifest_field(cj, "h", v, sizeof v) == 0 && atoi(v) > 0) h = atoi(v);
        if (store_manifest_field(cj, "encoding", v, sizeof v) == 0)
            mode = !strcmp(v, "y16le") ? MODE_RAW16 : !strcmp(v, "y8") ? MODE_Y8 : MODE_Y10P;
    }

    long n = 0;
    AirecIdxRow *rows = load_idx(dir, "eo_y10", &n);
    if (!rows || n <= 0) { free(rows); return -1; }

    double fps = 30.0;
    if (n > 1) {
        double dur = (rows[n - 1].t_ns - rows[0].t_ns) / 1e9;
        if (dur > 0) fps = (n - 1) / dur;
        if (fps < 1) fps = 1;
        if (fps > 120) fps = 120;
    }

    char tmp[700], cmd[1240];
    snprintf(tmp, sizeof tmp, "%s/native.mp4.%lu.tmp", dir, (unsigned long)pthread_self());
    snprintf(cmd, sizeof cmd,
        "nice -n 15 ionice -c3 ffmpeg -hide_banner -loglevel error -y "
        "-f rawvideo -pix_fmt gray -s %dx%d -r %.3f -i - "
        "-c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p "
        "-movflags +faststart -f mp4 '%s'", w, h, fps, tmp);
    FILE *ff = popen(cmd, "w");
    if (!ff) { free(rows); return -1; }

    uint8_t *out8 = malloc((size_t)w * h);
    uint8_t *raw = malloc((size_t)w * h * 2);
    EoToneState st = { 0, 0, 0 };
    FILE *cur = NULL; uint32_t cur_seg = 0xffffffff;   /* one segment open at a time */
    int ok = out8 && raw;

    /* frames are stored in segment order, so a single rolling file handle covers
     * ANY number of segments (no fixed cap that would truncate long recordings) */
    for (long i = 0; i < n && ok; i++) {
        AirecIdxRow *r = &rows[i];
        if (r->segment_no != cur_seg) {
            if (cur) fclose(cur);
            char sp[720];
            snprintf(sp, sizeof sp, "%s/eo_y10/data.%05u.airec", dir, r->segment_no);
            cur = fopen(sp, "rb"); cur_seg = r->segment_no;
            if (!cur) { ok = 0; break; }
        }
        if (fseek(cur, (long)r->offset + (long)sizeof(AirecRecHdr), SEEK_SET) != 0 ||
            r->payload_len > (uint32_t)w * h * 2 ||
            fread(raw, 1, r->payload_len, cur) != r->payload_len) continue;

        if (render_native_gray8(raw, r->payload_len, w, h, mode, 0, &st, i == 0, out8) == 0)
            if (fwrite(out8, 1, (size_t)w * h, ff) != (size_t)w * h) { ok = 0; break; }
        if (pct) *pct = (int)((i + 1) * 100 / n);
    }
    if (cur) fclose(cur);
    free(out8); free(raw); free(rows);
    int rc = pclose(ff);

    char final[680];
    snprintf(final, sizeof final, "%s/native.mp4", dir);
    if (ok && rc == 0 && rename(tmp, final) == 0) return 0;
    unlink(tmp);
    return -1;
}

static void *build_thread(void *arg)
{
    char sid[SID_LEN + 1];
    snprintf(sid, sizeof sid, "%s", (char *)arg);
    free(arg);
    int rc = build_mp4(sid, &g_job.pct);
    pthread_mutex_lock(&g_lk);
    if (!strcmp(g_job.sid, sid)) { g_job.state = rc == 0 ? 2 : -1; if (rc == 0) g_job.pct = 100; }
    pthread_mutex_unlock(&g_lk);
    if (rc != 0) fprintf(stderr, "transcode: %s failed\n", sid);
    return NULL;
}

/* Ensure native.mp4 exists (build it synchronously if missing). For export. */
int transcode_ensure(const char *sid)
{
    char p[640];
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return -1;
    if (transcode_mp4_path(sid, p, sizeof p) == 0) return 0;    /* already there */
    return build_mp4(sid, NULL);
}

void transcode_request(const char *sid)
{
    char p[640];
    if (transcode_mp4_path(sid, p, sizeof p) == 0) return;      /* already cached */
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return;

    pthread_mutex_lock(&g_lk);
    if (!strcmp(g_job.sid, sid) && g_job.state == 1) { pthread_mutex_unlock(&g_lk); return; }
    snprintf(g_job.sid, sizeof g_job.sid, "%s", sid);
    g_job.state = 1; g_job.pct = 0;
    char *arg = strdup(sid);
    if (pthread_create(&g_job.tid, NULL, build_thread, arg) == 0) pthread_detach(g_job.tid);
    else { g_job.state = -1; free(arg); }
    pthread_mutex_unlock(&g_lk);
}
