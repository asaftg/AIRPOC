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

static void *build_thread(void *arg)
{
    char sid[SID_LEN + 1];
    snprintf(sid, sizeof sid, "%s", (char *)arg);
    free(arg);

    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);

    /* geometry + encoding from channel.json */
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
    if (!rows || n <= 0) { free(rows); goto fail; }

    /* average fps from the frame timestamps (native is ~constant) */
    double fps = 30.0;
    if (n > 1) {
        double dur = (rows[n - 1].t_src_ns - rows[0].t_src_ns) / 1e9;
        if (dur > 0) fps = (n - 1) / dur;
        if (fps < 1) fps = 1;
        if (fps > 120) fps = 120;
    }

    /* ffmpeg: gray raw in -> H.264 mp4. -preset veryfast + crf 20 = good quality,
     * fast enough; niced so it yields to the live pipeline. faststart for <video>. */
    char tmp[680], cmd[1200];
    snprintf(tmp, sizeof tmp, "%s/native.mp4.tmp", dir);
    snprintf(cmd, sizeof cmd,
        "nice -n 15 ionice -c3 ffmpeg -hide_banner -loglevel error -y "
        "-f rawvideo -pix_fmt gray -s %dx%d -r %.3f -i - "
        "-c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p "
        "-movflags +faststart -f mp4 '%s'", w, h, fps, tmp);   /* -f mp4: .tmp ext */
    FILE *ff = popen(cmd, "w");
    if (!ff) { free(rows); goto fail; }

    uint8_t *out8 = malloc((size_t)w * h);
    uint8_t *raw = malloc(mode == MODE_RAW16 ? (size_t)w * h * 2 : (size_t)w * h * 2);
    EoToneState st = { 0, 0, 0 };
    FILE *segs[64] = { 0 };
    int ok = out8 && raw;

    for (long i = 0; i < n && ok; i++) {
        AirecIdxRow *r = &rows[i];
        if (r->segment_no >= 64) continue;
        if (!segs[r->segment_no]) {
            char sp[720];
            snprintf(sp, sizeof sp, "%s/eo_y10/data.%05u.airec", dir, r->segment_no);
            segs[r->segment_no] = fopen(sp, "rb");
            if (!segs[r->segment_no]) { ok = 0; break; }
        }
        FILE *sf = segs[r->segment_no];
        if (fseek(sf, (long)r->offset + (long)sizeof(AirecRecHdr), SEEK_SET) != 0 ||
            r->payload_len > (mode == MODE_RAW16 ? (uint32_t)w * h * 2 : (uint32_t)w * h * 2) ||
            fread(raw, 1, r->payload_len, sf) != r->payload_len) continue;   /* skip torn frame */

        if (render_native_gray8(raw, r->payload_len, w, h, mode, 0, &st, i == 0, out8) == 0)
            if (fwrite(out8, 1, (size_t)w * h, ff) != (size_t)w * h) { ok = 0; break; }

        int pct = (int)((i + 1) * 100 / n);
        pthread_mutex_lock(&g_lk);
        if (!strcmp(g_job.sid, sid)) g_job.pct = pct;
        pthread_mutex_unlock(&g_lk);
    }
    for (int s = 0; s < 64; s++) if (segs[s]) fclose(segs[s]);
    free(out8); free(raw); free(rows);
    int rc = pclose(ff);

    char final[680];
    snprintf(final, sizeof final, "%s/native.mp4", dir);
    if (ok && rc == 0 && rename(tmp, final) == 0) {
        pthread_mutex_lock(&g_lk);
        if (!strcmp(g_job.sid, sid)) { g_job.state = 2; g_job.pct = 100; }
        pthread_mutex_unlock(&g_lk);
        return NULL;
    }
    unlink(tmp);
fail:
    pthread_mutex_lock(&g_lk);
    if (!strcmp(g_job.sid, sid)) g_job.state = -1;
    pthread_mutex_unlock(&g_lk);
    fprintf(stderr, "transcode: %s failed\n", sid);
    return NULL;
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
