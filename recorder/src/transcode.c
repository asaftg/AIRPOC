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
#include <dirent.h>

/* Delete orphaned encode temp files (native.mp4.<tid>.tmp) in one session dir —
 * left behind when a transcode is SIGKILL'd (normal cancel unlinks its own). */
static void cleanup_tmp_dir(const char *sdir)
{
    DIR *sd = opendir(sdir);
    if (!sd) return;
    struct dirent *f;
    while ((f = readdir(sd))) {
        size_t l = strlen(f->d_name);
        if (strncmp(f->d_name, "native.mp4.", 11) == 0 && l > 4 &&
            strcmp(f->d_name + l - 4, ".tmp") == 0) {
            char fp[1400];
            snprintf(fp, sizeof fp, "%s/%s", sdir, f->d_name);
            unlink(fp);
        }
    }
    closedir(sd);
}

/* Startup sweep: clear stale encode temps across all sessions. */
void transcode_cleanup_tmp(void)
{
    DIR *root = opendir(g_rec.root);
    if (!root) return;
    struct dirent *e;
    while ((e = readdir(root))) {
        if (e->d_name[0] == '.') continue;
        char sdir[700];
        snprintf(sdir, sizeof sdir, "%s/%s", g_rec.root, e->d_name);
        cleanup_tmp_dir(sdir);
    }
    closedir(root);
}

typedef struct {
    char sid[SID_LEN + 1];
    int  state;                 /* 2 ready, 1 building, 0 none, -1 failed */
    int  pct;
    pthread_t tid;
} Job;

static Job g_job;               /* one at a time (one replay open at a time) */
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;

/* One encode runs at a time (~2 cores), never a stack. Concurrent requests for
 * DIFFERENT sessions are queued 1-deep (g_pending) and run to completion in turn —
 * NOT cancelled — so two operators converting two clips don't ping-pong forever.
 * g_txgen is the abort token: a build captures it and aborts its frame loop the
 * moment it changes (ffmpeg then hits stdin EOF and exits — prompt, not an instant
 * signal-kill). Only transcode_cancel() bumps it (explicit stop). */
static volatile unsigned g_txgen = 0;
typedef struct { char sid[SID_LEN + 1]; unsigned gen; } BuildArg;
static char g_pending[SID_LEN + 1] = "";     /* next sid to build when the running one finishes */

void transcode_cancel(void)
{
    pthread_mutex_lock(&g_lk);
    g_txgen++;                   /* in-flight build aborts on its next frame */
    g_pending[0] = 0;
    if (g_job.state == 1) g_job.state = 0;
    pthread_mutex_unlock(&g_lk);
}

/* Bump when the encode parameters change so already-cached mp4s auto-rebuild.
 * v1 = uncapped (violated H.264 level, stalled decoders).
 * v2 = capped 20 Mbit/s (fixed stall but too soft).
 * v3 = 50 Mbit/s + keyframe/s — high quality, still level-conformant, smooth seek.
 * v4 = (median-flag attempt — superseded).
 * v5 = hqdn3d denoise: kills night-grain shimmer that H.264 introduced.
 * v6 = meter the tone map on the operator's recorded zoom crop (no full-frame blow-out).
 * v7 = PER-FRAME zoom crop (zoom varies within a session; session-level blew out wide parts). */
#define TRANSCODE_VER 7

/* An mp4 is usable only if it exists AND carries the current encoder version
 * (sidecar native.mp4.ver). Older recordings' mp4s have no marker (or a stale
 * one), so they are treated as needing a rebuild and regenerate on open — that's
 * how pre-existing movies get the fix, not just new recordings. */
static int mp4_current(const char *sid)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return 0;
    char p[680];
    snprintf(p, sizeof p, "%s/%s/native.mp4", g_rec.root, sid);
    if (access(p, F_OK) != 0) return 0;
    snprintf(p, sizeof p, "%s/%s/native.mp4.ver", g_rec.root, sid);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int ver = 0;
    if (fscanf(f, "%d", &ver) != 1) ver = 0;
    fclose(f);
    return ver == TRANSCODE_VER;
}

int transcode_mp4_path(const char *sid, char *path, size_t plen)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return -1;
    snprintf(path, plen, "%s/%s/native.mp4", g_rec.root, sid);
    /* Must be a real, playable file -- a zero-byte leftover is not "HD ready".
     * (Builds go to native.mp4.<pid>.tmp and are renamed in, so a .tmp can
     * never be seen under this name.) */
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) ? 0 : -1;
}

int transcode_status(const char *sid, int *pct)
{
    /* "ready" = a playable mp4 is on disk, current OR a stale pre-cap build. A
     * stale file is upgraded invisibly in the background (see the serve handler);
     * it must NOT report "building", or the console hides native the whole time a
     * perfectly playable file is sitting right there. "building" means truly no
     * file yet. */
    char p[640];
    if (transcode_mp4_path(sid, p, sizeof p) == 0) { if (pct) *pct = 100; return 2; }
    pthread_mutex_lock(&g_lk);
    int st = (!strcmp(g_job.sid, sid)) ? g_job.state : 0;
    if (pct) *pct = (!strcmp(g_job.sid, sid)) ? g_job.pct : 0;
    pthread_mutex_unlock(&g_lk);
    return st;
}

/* For the explicit "Convert to native (HD)" button: is the CURRENT-quality mp4
 * ready (2), building now (1), failed (-1), or not started (0)? Distinct from
 * transcode_status, which reports "ready" for any playable file incl. a stale one. */
int transcode_current_state(const char *sid, int *pct)
{
    if (mp4_current(sid)) { if (pct) *pct = 100; return 2; }
    pthread_mutex_lock(&g_lk);
    int st = 0;
    if (!strcmp(g_job.sid, sid)) st = g_job.state == 1 ? 1 : g_job.state == -1 ? -1 : 0;
    if (pct) *pct = (st == 1) ? g_job.pct : 0;
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

/* Per-frame operator-zoom timeline from eo_jpeg (meta {seq,dw,dh,zoom,res,0}), so
 * each mp4 frame is tone-mapped for the crop the operator viewed at THAT instant —
 * zoom varies within a session, and metering a wide frame on a narrow crop blows
 * out the periphery. Index order == time order (0 => whole-frame at that point). */
typedef struct { uint64_t t; int z, dw, dh; } ZoomPt;

static ZoomPt *build_zoom_timeline(const char *dir, long *out_n)
{
    long n = 0;
    AirecIdxRow *rows = load_idx(dir, "eo_jpeg", &n);
    *out_n = 0;
    if (!rows || n <= 0) { free(rows); return NULL; }
    ZoomPt *zt = malloc((size_t)n * sizeof *zt);
    if (!zt) { free(rows); return NULL; }
    FILE *f = NULL; uint32_t seg = 0xffffffff;
    for (long k = 0; k < n; k++) {
        AirecIdxRow *r = &rows[k];
        zt[k].t = r->t_ns; zt[k].z = 0; zt[k].dw = 0; zt[k].dh = 0;
        if (r->segment_no != seg) {
            if (f) fclose(f);
            char sp[720]; snprintf(sp, sizeof sp, "%s/eo_jpeg/data.%05u.airec", dir, r->segment_no);
            f = fopen(sp, "rb"); seg = r->segment_no;
        }
        AirecRecHdr h;
        if (f && fseek(f, (long)r->offset, SEEK_SET) == 0 &&
            fread(&h, 1, sizeof h, f) == sizeof h && h.magic == AIREC_REC_MAGIC) {
            zt[k].dw = (int)h.meta[1]; zt[k].dh = (int)h.meta[2]; zt[k].z = (int)h.meta[3];
        }
    }
    if (f) fclose(f);
    free(rows);
    *out_n = n;
    return zt;
}

static void zoom_at(const ZoomPt *zt, long n, uint64_t t, int *z, int *dw, int *dh)
{
    *z = 0; *dw = 0; *dh = 0;
    if (!zt || n <= 0) return;
    long lo = 0, hi = n - 1, best = 0;
    while (lo <= hi) { long mid = (lo + hi) / 2; if (zt[mid].t <= t) { best = mid; lo = mid + 1; } else hi = mid - 1; }
    *z = zt[best].z; *dw = zt[best].dw; *dh = zt[best].dh;
}

/* Build native.mp4 for a session (blocking). pct (optional) tracks progress.
 * Unique tmp per caller so a replay-triggered build and an export build can't
 * clobber each other. Returns 0 on success. */
static int build_mp4(const char *sid, volatile int *pct, unsigned gen)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);
    cleanup_tmp_dir(dir);        /* clear any orphaned temp from a prior killed build (cap-1: none live) */

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
        /* Lowest priority the OS offers: nice 19 (minimum share of CPU whenever
         * anything else wants it), ionice idle (only touches the NVMe when no one
         * else is), and -threads 2 of 6 cores. The thread cap matters as much as
         * the nice level: nice throttles CPU share but NOT memory bandwidth, and
         * the EO pipeline is memory-bound at 188 MB/s -- a 6-thread x264 encode
         * would starve it on bandwidth no matter how politely it was scheduled. */
        "nice -n 19 ionice -c3 ffmpeg -threads 2 -hide_banner -loglevel error -y "
        "-f rawvideo -pix_fmt gray -s %dx%d -r %.3f -i - "
        /* Denoise before encoding: night sensor grain is mostly temporal (differs
         * each frame). H.264 turns it into visible frame-to-frame "shimmer" the raw
         * live view doesn't have, AND wastes bits fighting it (soft result). hqdn3d
         * (spatial+temporal) removes the grain -> no shimmer + the freed bits go to
         * real detail = sharper. Full unfiltered detail is still on the raw frames at
         * pause/step. */
        "-vf hqdn3d "
        /* Quality vs. conformance: plain -crf on grainy night footage hit ~118 Mbit/s,
         * which BLOWS PAST the H.264 High L4.2 ceiling (~62.5 Mbit/s) the stream
         * declares — a level violation that stalled browser decoders mid-playback.
         * An earlier 20 Mbit/s cap fixed the stall but softened detail (bad for
         * reading a target at range). Cap at 50 Mbit/s: high quality, still inside
         * L4.2 (bufsize 72M < the ~78 Mbit CPB) so it stays conformant and never
         * stalls. `-g 60` = a keyframe every ~1 s so scrub/seek is smooth. Full
         * detail is still available on pause/step (rendered from the raw frames). */
        "-c:v libx264 -preset veryfast -crf 18 -maxrate 50M -bufsize 72M -g 60 "
        "-pix_fmt yuv420p -movflags +faststart -f mp4 '%s'", w, h, fps, tmp);
    FILE *ff = popen(cmd, "w");
    if (!ff) { free(rows); return -1; }

    uint8_t *out8 = malloc((size_t)w * h);
    uint8_t *raw = malloc((size_t)w * h * 2);
    EoToneState st = { 0, 0, 0 };
    long zn = 0; ZoomPt *zt = build_zoom_timeline(dir, &zn);   /* per-frame operator zoom for tone-map metering */
    FILE *cur = NULL; uint32_t cur_seg = 0xffffffff;   /* one segment open at a time */
    int ok = out8 && raw;

    /* frames are stored in segment order, so a single rolling file handle covers
     * ANY number of segments (no fixed cap that would truncate long recordings) */
    for (long i = 0; i < n && ok; i++) {
        if (g_txgen != gen) { ok = 0; break; }   /* cancelled (replay closed / newer build) */
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

        int mz, mdw, mdh; zoom_at(zt, zn, r->t_ns, &mz, &mdw, &mdh);   /* the crop the operator viewed at this frame */
        if (render_native_gray8(raw, r->payload_len, w, h, mode, 0, &st, i == 0, out8, mz, mdw, mdh) == 0)
            if (fwrite(out8, 1, (size_t)w * h, ff) != (size_t)w * h) { ok = 0; break; }
        if (pct) *pct = (int)((i + 1) * 100 / n);
    }
    if (cur) fclose(cur);
    free(out8); free(raw); free(rows); free(zt);
    int rc = pclose(ff);

    char final[680];
    snprintf(final, sizeof final, "%s/native.mp4", dir);
    if (ok && rc == 0 && rename(tmp, final) == 0) {
        char vp[700];                               /* stamp the encoder version */
        snprintf(vp, sizeof vp, "%s/native.mp4.ver", dir);
        FILE *vf = fopen(vp, "w");
        if (vf) { fprintf(vf, "%d\n", TRANSCODE_VER); fclose(vf); }
        return 0;
    }
    unlink(tmp);
    return -1;
}

static void *build_thread(void *arg);            /* forward decl (start_build_locked spawns it) */

/* Start a build for sid. MUST be called with g_lk held, and only when no build is
 * currently running (state != 1). Gives the build a fresh abort token. */
static void start_build_locked(const char *sid)
{
    unsigned gen = ++g_txgen;
    snprintf(g_job.sid, sizeof g_job.sid, "%s", sid);
    g_job.state = 1; g_job.pct = 0;
    BuildArg *arg = malloc(sizeof *arg);
    if (arg) { snprintf(arg->sid, sizeof arg->sid, "%s", sid); arg->gen = gen; }
    if (arg && pthread_create(&g_job.tid, NULL, build_thread, arg) == 0) pthread_detach(g_job.tid);
    else { g_job.state = -1; free(arg); }
}

static void *build_thread(void *arg)
{
    BuildArg *ba = arg;
    char sid[SID_LEN + 1];
    snprintf(sid, sizeof sid, "%s", ba->sid);
    unsigned gen = ba->gen;
    free(ba);
    int rc = build_mp4(sid, &g_job.pct, gen);
    pthread_mutex_lock(&g_lk);
    /* record the result + drain the queue only if we're still the current job (a
     * superseding build owns g_job and manages its own queue). */
    if (!strcmp(g_job.sid, sid) && g_txgen == gen) {
        g_job.state = rc == 0 ? 2 : -1;
        if (rc == 0) g_job.pct = 100;
        if (g_pending[0]) {                          /* 1-deep queue: start the next requested build */
            char next[SID_LEN + 1];
            snprintf(next, sizeof next, "%s", g_pending);
            g_pending[0] = 0;
            if (strcmp(next, sid) != 0 && !mp4_current(next)) start_build_locked(next);
        }
    }
    pthread_mutex_unlock(&g_lk);
    if (rc != 0) fprintf(stderr, "transcode: %s failed/cancelled\n", sid);
    return NULL;
}

/* ---- background catch-up builder ----
 *
 * Every saved session should end up with an HD movie. session_save() already asks
 * for one, but the request was silently dropped: the queue is 1-deep, so saving
 * several clips in a row built the first, pended the second, and threw the rest
 * away. 10 of 19 sessions in one day ended up with no movie that way.
 *
 * Scanning for what is actually MISSING fixes that class of bug outright -- a lost
 * request no longer matters, and the existing backlog heals itself. Two hard rules:
 *
 *   1. NEVER while recording. This refuses to start one, and session_start()
 *      cancels an in-flight build, so pressing REC stops the encoder mid-frame.
 *   2. Lowest priority (see the ffmpeg line above).
 *
 * Only MISSING movies are built, never stale-version ones: a stale movie already
 * plays and is upgraded on open, so rebuilding 26 of them would burn an hour of
 * CPU to replace files that already work.
 */
#define AUTOBUILD_TICK_S   20
#define AUTOBUILD_MAX_SIDS 512
#define AUTOBUILD_MAX_SKIP 64

static char g_skip[AUTOBUILD_MAX_SKIP][SID_LEN + 1];   /* builds that failed; don't loop on them */
static int  g_nskip = 0;

static int autobuild_skipped(const char *sid)
{
    for (int i = 0; i < g_nskip; i++) if (!strcmp(g_skip[i], sid)) return 1;
    return 0;
}

/* A session wants a movie if it is finished, has native frames to render, and
 * has no mp4 yet. */
static int autobuild_wants(const char *sid)
{
    char dir[640], p[720], mf[24576], state[32] = "";
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);
    snprintf(p, sizeof p, "%s/native.mp4", dir);
    if (access(p, F_OK) == 0) return 0;                       /* already has one */
    snprintf(p, sizeof p, "%s/eo_y10/index.bin", dir);
    if (access(p, F_OK) != 0) return 0;                       /* nothing to render */
    if (store_manifest_read(dir, mf, sizeof mf) < 0) return 0;
    store_manifest_field(mf, "state", state, sizeof state);
    return !strcmp(state, "saved") || !strcmp(state, "recovered");
}

static void *autobuild_thread(void *arg)
{
    (void)arg;
    static char sids[AUTOBUILD_MAX_SIDS][SID_LEN + 1];
    char kicked[SID_LEN + 1] = "";
    int idle_ticks = 0;

    fprintf(stderr, "rec: HD catch-up worker started (idle-only, %ds tick)\n", AUTOBUILD_TICK_S);

    for (;;) {
        sleep(AUTOBUILD_TICK_S);
        if (chan_recording()) continue;                       /* rule 1 */

        pthread_mutex_lock(&g_lk);
        int busy = (g_job.state == 1);
        pthread_mutex_unlock(&g_lk);
        if (busy) {
            /* An operator-requested build (opening a movie) is running -- wait it
             * out rather than queue behind it. Say so occasionally: a silently
             * yielding worker is indistinguishable from a dead one. */
            if (++idle_ticks % 15 == 0) fprintf(stderr, "rec: HD catch-up waiting (a build is running)\n");
            continue;
        }
        idle_ticks = 0;

        /* The build we kicked last tick is done. If it produced nothing, stop
         * retrying it forever -- otherwise one unencodable session blocks every
         * session behind it. */
        if (kicked[0]) {
            char p[720];
            snprintf(p, sizeof p, "%s/%s/native.mp4", g_rec.root, kicked);
            if (access(p, F_OK) != 0 && g_nskip < AUTOBUILD_MAX_SKIP)
                snprintf(g_skip[g_nskip++], SID_LEN + 1, "%.*s", SID_LEN, kicked);
            kicked[0] = 0;
        }

        int n = store_list_sids(g_rec.root, sids, AUTOBUILD_MAX_SIDS);
        for (int i = 0; i < n; i++) {
            if (autobuild_skipped(sids[i]) || !autobuild_wants(sids[i])) continue;
            if (chan_recording()) break;                       /* rule 1, re-checked */
            fprintf(stderr, "rec: auto-building HD for %s (%d skipped)\n", sids[i], g_nskip);
            snprintf(kicked, sizeof kicked, "%.*s", SID_LEN, sids[i]);
            transcode_request(sids[i]);
            break;                                             /* one at a time */
        }
    }
    return NULL;
}

void transcode_autobuild_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, autobuild_thread, NULL) == 0) pthread_detach(t);
}

void transcode_request(const char *sid)
{
    if (mp4_current(sid)) return;                              /* current build already cached */
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return;

    pthread_mutex_lock(&g_lk);
    if (g_job.state == 1) {
        /* A build is running. Queue a DIFFERENT session 1-deep instead of
         * cancelling it — cancelling on every poll made two concurrent conversions
         * ping-pong and never finish. Same sid already building → nothing to do. */
        if (strcmp(g_job.sid, sid) != 0) snprintf(g_pending, sizeof g_pending, "%s", sid);
        pthread_mutex_unlock(&g_lk);
        return;
    }
    /* Nothing running. Don't re-kick a build that just FAILED for this same sid —
     * else every <video> range request re-storms ffmpeg on a session whose encode
     * can't succeed. It retries once another session builds (clears g_job.sid). */
    if (!strcmp(g_job.sid, sid) && g_job.state == -1) {
        pthread_mutex_unlock(&g_lk);
        return;
    }
    start_build_locked(sid);
    pthread_mutex_unlock(&g_lk);
}
