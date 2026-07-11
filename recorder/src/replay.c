/* replay.c — timeline-paced playback of a recorded session through the same
 * endpoint shapes the browser already polls. Zero decode/encode: display JPEGs
 * and radar JSON are served byte-verbatim from read-only mmaps.
 *
 * One session open at a time; open replaces. Pushers (one per /replay/stream
 * connection) key on a generation counter and exit promptly when it changes.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include "eo_tonemap.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MAX_EVS  65536

typedef struct { uint8_t *map; size_t len; } Seg;

typedef struct {
    AirecIdxRow *rows;
    int64_t *t_ms;                       /* per row, session-relative */
    long n;
    Seg *segs;                           /* dynamic: a long recording has 100s of segments */
    int nsegs;
} RChan;

typedef struct { int64_t t_ms; const char *body; int len; int type; } Ev;
enum { EV_EO, EV_RD, EV_APP };

static struct {
    pthread_mutex_t lk;
    pthread_cond_t cv;
    int open;
    unsigned gen;
    int pushers;
    char sid[SID_LEN + 1], name[NAME_MAX_LEN * 2];
    uint64_t t0_mono, t0_real;
    int64_t dur_ms;
    /* transport */
    int64_t t_ms;
    double rate;
    int playing;
    uint64_t anchor_ns;
    /* data */
    RChan jpeg, radar, y10, det;
    int has_native, has_display, video_native;    /* video source selection */
    int nat_w, nat_h, nat_mode;                    /* native geometry + encoding */
    int tonemap_match;                             /* replay tone map == record-time tone map */
    int tm_vs_eo;                                  /* 0 not checked, 1 matches EO, -1 drift */
    double tm_vs_eo_diff;                          /* mean abs pixel diff of the check */
    int has_illum;                                 /* eo_y10 meta[4] carries packed illuminator */
    int play_q;                                    /* native JPEG quality while playing */
    double play_fps;                               /* native frame cap while playing */
    char *evbuf;                         /* events channel, loaded whole */
    Ev evs[MAX_EVS];
    int n_evs;
} g_rp = { .lk = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .rate = 1.0 };

/* native-render cache + tonemap EMA: shared across pushers/viewers, so the
 * encode happens once per distinct frame and the anti-breathing EMA advances
 * like the live feed (sequential frames) or re-seeds on a seek/jump. */
static struct {
    pthread_mutex_t lk;
    long idx;                            /* eo_y10 row index cached, -1 = none */
    int q;                               /* quality the cached frame was encoded at */
    uint8_t *jpg;
    uint32_t len;
    EoToneState tone;                    /* EMA state, reflects tone_i */
    long tone_i;                         /* frame index `tone` currently reflects */
} g_nat = { .lk = PTHREAD_MUTEX_INITIALIZER, .idx = -1, .q = -1, .tone_i = -2 };

#define NAT_FULL_Q 85                    /* pause/step/scrub: full native detail */

#define NAT_JPG_CAP (1u << 20)          /* matches the http serve buffer */

/* the channel driving the video timeline: native if selected+present, else display */
static RChan *video_chan(void)
{
    return (g_rp.video_native && g_rp.has_native) ? &g_rp.y10 : &g_rp.jpeg;
}

static const Ev *ev_at(int type, int64_t t);       /* defined below */
static int ev_int(const Ev *e, const char *key, int dflt);  /* defined below */

/* ---- loading ---- */

static void rchan_free(RChan *c)
{
    free(c->rows); free(c->t_ms);
    for (int i = 0; i < c->nsegs; i++)
        if (c->segs[i].map) munmap(c->segs[i].map, c->segs[i].len);
    free(c->segs);
    memset(c, 0, sizeof *c);
}

static int rchan_load(const char *dir, const char *name, RChan *c, uint64_t *t0_out)
{
    memset(c, 0, sizeof *c);
    char path[720];
    snprintf(path, sizeof path, "%s/%s/index.bin", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    c->n = sz / (long)sizeof(AirecIdxRow);
    if (c->n <= 0) { fclose(f); return -1; }
    c->rows = malloc((size_t)sz);
    c->t_ms = malloc((size_t)c->n * sizeof(int64_t));
    if (!c->rows || !c->t_ms || fread(c->rows, sizeof(AirecIdxRow), (size_t)c->n, f) != (size_t)c->n) {
        fclose(f); rchan_free(c); return -1;
    }
    fclose(f);

    /* segment count = highest segment_no in the index + 1 (a long recording has
     * hundreds of 256 MiB segments — must map them ALL or replay truncates) */
    int nseg = 0;
    for (long i = 0; i < c->n; i++)
        if ((int)c->rows[i].segment_no + 1 > nseg) nseg = c->rows[i].segment_no + 1;
    c->segs = calloc((size_t)nseg, sizeof(Seg));
    if (!c->segs) { rchan_free(c); return -1; }

    for (int s = 0; s < nseg; s++) {
        snprintf(path, sizeof path, "%s/%s/data.%05d.airec", dir, name, s);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;                /* a hole shouldn't happen; skip gracefully */
        struct stat st;
        fstat(fd, &st);
        c->segs[s].len = (size_t)st.st_size;
        c->segs[s].map = mmap(NULL, c->segs[s].len, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (c->segs[s].map == MAP_FAILED) c->segs[s].map = NULL;
        c->nsegs = s + 1;
    }
    if (!c->nsegs || !c->segs[0].map) { rchan_free(c); return -1; }

    uint64_t t0 = ((AirecSegHdr *)c->segs[0].map)->session_t0_mono_ns;
    if (t0_out) *t0_out = t0;
    for (long i = 0; i < c->n; i++)
        c->t_ms[i] = (int64_t)((c->rows[i].t_ns - t0) / 1000000ull);
    return 0;
}

/* last row with t_ms <= t; -1 if before first */
static long rchan_at(const RChan *c, int64_t t)
{
    if (!c->n || t < c->t_ms[0]) return c->n ? -1 : -1;
    long lo = 0, hi = c->n - 1;
    while (lo < hi) {
        long mid = (lo + hi + 1) / 2;
        if (c->t_ms[mid] <= t) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static const uint8_t *rchan_payload(const RChan *c, long i, uint32_t *len, const AirecRecHdr **hdr)
{
    const AirecIdxRow *r = &c->rows[i];
    if (r->segment_no >= (uint32_t)c->nsegs) return NULL;
    const Seg *s = &c->segs[r->segment_no];
    if ((size_t)r->offset + sizeof(AirecRecHdr) + r->payload_len > s->len) return NULL;
    const AirecRecHdr *h = (const AirecRecHdr *)(s->map + r->offset);
    if (h->magic != AIREC_REC_MAGIC) return NULL;
    if (hdr) *hdr = h;
    *len = r->payload_len;
    return s->map + r->offset + sizeof(AirecRecHdr);
}

static void load_events(const char *dir)
{
    g_rp.n_evs = 0;
    free(g_rp.evbuf);
    g_rp.evbuf = NULL;

    RChan ev;
    if (rchan_load(dir, "events", &ev, NULL) != 0) return;
    /* copy whole payload space into one buffer so we can drop the mmaps */
    size_t total = 0;
    for (long i = 0; i < ev.n; i++) total += ev.rows[i].payload_len + 1;
    g_rp.evbuf = malloc(total + 1);
    size_t o = 0;
    for (long i = 0; i < ev.n && g_rp.n_evs < MAX_EVS; i++) {
        uint32_t plen;
        const uint8_t *p = rchan_payload(&ev, i, &plen, NULL);
        if (!p) continue;
        char *dst = g_rp.evbuf + o;
        memcpy(dst, p, plen);
        dst[plen] = 0;
        o += plen + 1;

        int type = -1;
        if (strstr(dst, "\"type\":\"eo_stats\"")) type = EV_EO;
        else if (strstr(dst, "\"type\":\"radar_stats\"")) type = EV_RD;
        else if (strstr(dst, "\"type\":\"app_stats\"")) type = EV_APP;
        if (type < 0) continue;
        const char *body = strstr(dst, "\"body\":");
        if (!body) continue;
        body += 7;
        int blen = (int)(plen - (size_t)(body - dst));
        while (blen > 0 && (body[blen - 1] == '\n' || body[blen - 1] == '\r' ||
                            body[blen - 1] == ' ')) blen--;
        if (blen > 0 && body[blen - 1] == '}') blen--;        /* the EVENT's closing }, */
        while (blen > 0 && (body[blen - 1] == '\n' || body[blen - 1] == ' ')) blen--;
        if (blen <= 2 || body[0] != '{' || body[blen - 1] != '}') continue;
        Ev *e = &g_rp.evs[g_rp.n_evs++];
        e->t_ms = ev.t_ms[i];
        e->body = body;
        e->len = blen;
        e->type = type;
    }
    rchan_free(&ev);
}

static const Ev *ev_at(int type, int64_t t)
{
    const Ev *best = NULL, *first = NULL;
    for (int i = 0; i < g_rp.n_evs; i++) {
        if (g_rp.evs[i].type != type) continue;
        if (!first) first = &g_rp.evs[i];       /* earliest of this type */
        if (g_rp.evs[i].t_ms <= t) best = &g_rp.evs[i];
    }
    return best ? best : first;                  /* before first snapshot: hold it */
}

/* ---- transport clock (call with lk held) ---- */

static int64_t clock_now(void)
{
    int64_t t = g_rp.t_ms;
    if (g_rp.playing)
        t += (int64_t)((now_ns() - g_rp.anchor_ns) / 1e6 * g_rp.rate);
    if (t >= g_rp.dur_ms) {
        t = g_rp.dur_ms;
        g_rp.t_ms = t;
        g_rp.playing = 0;                /* auto-pause at end */
    }
    if (t < 0) t = 0;
    return t;
}

static void clock_set(int64_t t, int playing)
{
    if (t < 0) t = 0;
    if (t > g_rp.dur_ms) t = g_rp.dur_ms;
    g_rp.t_ms = t;
    g_rp.playing = playing;
    g_rp.anchor_ns = now_ns();
    pthread_cond_broadcast(&g_rp.cv);
}

/* Auto drift check: does the recorder's native tone map still match the LIVE EO
 * feed? Compare one native frame (my tone map) against the operator's recorded
 * display JPEG (EO's real tone map) at a zoom=1 frame, downscaled to match.
 * ~10 ms, once at open. Sets tm_vs_eo: 1 match, -1 drift, 0 couldn't check. */
#define TM_DRIFT_THRESH 8.0                 /* mean abs 8-bit diff above JPEG noise */

static void tonemap_verify_vs_display(void)
{
    g_rp.tm_vs_eo = 0;
    g_rp.tm_vs_eo_diff = 0;
    if (!g_rp.has_native || !g_rp.has_display || g_rp.jpeg.n == 0) return;

    /* pick a zoom=1 display frame ~1/4 into the session */
    long start = g_rp.jpeg.n / 4, pick = -1;
    int dw = 0, dh = 0;
    const uint8_t *djpg = NULL; uint32_t djlen = 0;
    for (long k = 0; k < g_rp.jpeg.n; k++) {
        long i = (start + k) % g_rp.jpeg.n;
        const AirecRecHdr *h;
        uint32_t l;
        const uint8_t *p = rchan_payload(&g_rp.jpeg, i, &l, &h);
        if (p && h && h->meta[3] == 1 && h->meta[1] > 0 && h->meta[2] > 0) {  /* zoom==1 */
            pick = i; dw = (int)h->meta[1]; dh = (int)h->meta[2]; djpg = p; djlen = l; break;
        }
    }
    if (pick < 0) return;                             /* always zoomed: skip */

    /* native frame at the same instant */
    long ni = rchan_at(&g_rp.y10, g_rp.jpeg.t_ms[pick]);
    if (ni < 0) ni = 0;

    int nw = g_rp.nat_w, nh = g_rp.nat_h;
    uint8_t *disp = malloc((size_t)dw * dh);
    uint8_t *nat  = malloc((size_t)nw * nh);
    EoToneState st = { 0, 0, 0 };
    int ddw = 0, ddh = 0;
    /* Match the display JPEG exactly at the compare frame:
     *  - Warm my tone-map EMA to the phase the live feed's was in when it wrote
     *    the JPEG: seed ~16 frames back and advance forward so the stretch
     *    endpoints settle. A cold single-frame seed vs EO's continuously-warm EMA
     *    reads as drift on any brightness transition (e.g. the illuminator).
     *  - Apply the same 3x3 median grain filter the live view had (median=1 in
     *    low light). The display JPEG is tone-map THEN median; rendering the
     *    compare frame without it mismatches by ~10 counts on grainy night
     *    footage — a false "drift" even though playback (nat_jpeg) renders median
     *    correctly. Only the compare frame (wi==ni) needs it; warm-up frames just
     *    settle the EMA, which is computed pre-median. */
    int median = ev_int(ev_at(EV_EO, g_rp.jpeg.t_ms[pick]), "median", 0);
    int warm_ok = 0;
    if (disp && nat &&
        render_decode_jpeg_gray(djpg, djlen, disp, (uint32_t)dw * dh, &ddw, &ddh) == 0 &&
        ddw == dw && ddh == dh) {
        long w0 = ni - 16; if (w0 < 0) w0 = 0;
        int seeded = 0;
        for (long wi = w0; wi <= ni; wi++) {
            uint32_t wpl;
            const uint8_t *wraw = rchan_payload(&g_rp.y10, wi, &wpl, NULL);
            if (!wraw) continue;
            warm_ok = render_native_gray8(wraw, wpl, nw, nh, g_rp.nat_mode,
                                          wi == ni ? median : 0,
                                          &st, !seeded, nat) == 0;
            seeded = 1;
            if (!warm_ok) break;
        }
    }
    if (warm_ok) {
        /* box-average downscale native -> display res (matches EO at zoom=1) */
        double sum = 0;
        for (int oy = 0; oy < dh; oy++) {
            int sy0 = oy * nh / dh, sy1 = (oy + 1) * nh / dh; if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int ox = 0; ox < dw; ox++) {
                int sx0 = ox * nw / dw, sx1 = (ox + 1) * nw / dw; if (sx1 <= sx0) sx1 = sx0 + 1;
                unsigned acc = 0, cnt = 0;
                for (int sy = sy0; sy < sy1; sy++)
                    for (int sx = sx0; sx < sx1; sx++) { acc += nat[(size_t)sy * nw + sx]; cnt++; }
                int v = cnt ? (int)(acc / cnt) : 0;
                sum += abs(v - disp[(size_t)oy * dw + ox]);
            }
        }
        double mean = sum / ((double)dw * dh);
        g_rp.tm_vs_eo_diff = mean;
        g_rp.tm_vs_eo = mean > TM_DRIFT_THRESH ? -1 : 1;
        if (g_rp.tm_vs_eo < 0)
            fprintf(stderr, "replay: tone map DRIFT vs EO feed (mean diff %.1f) — mirror eo_tonemap.c\n", mean);
    }
    free(disp); free(nat);
}

/* ---- open/close ---- */

static void nat_cache_drop(void)
{
    pthread_mutex_lock(&g_nat.lk);
    free(g_nat.jpg);
    g_nat.jpg = NULL;
    g_nat.idx = -1;
    g_nat.tone_i = -2;
    memset(&g_nat.tone, 0, sizeof g_nat.tone);
    pthread_mutex_unlock(&g_nat.lk);
}

static int ev_int(const Ev *e, const char *key, int dflt)
{
    if (!e) return dflt;
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = e->body;
    for (int i = 0; i < e->len - (int)strlen(pat); i++)
        if (!strncmp(p + i, pat, strlen(pat))) return atoi(p + i + strlen(pat));
    return dflt;
}

/* Native JPEG for eo_y10 row i via the SHARED EO tone map, 1-frame cache.
 * Never holds two locks at once (avoids the g_rp<->g_nat ordering deadlock):
 * cache check (g_nat), payload+params copy (g_rp), render+cache (g_nat).
 * Caller must NOT hold g_rp.lk. */
static int nat_jpeg(long i, int quality, uint8_t *buf, uint32_t *len)
{
    pthread_mutex_lock(&g_nat.lk);
    if (g_nat.idx == i && g_nat.q == quality && g_nat.jpg) {
        memcpy(buf, g_nat.jpg, g_nat.len);
        *len = g_nat.len;
        pthread_mutex_unlock(&g_nat.lk);
        return 0;
    }
    pthread_mutex_unlock(&g_nat.lk);

    /* copy raw payload + params out from under g_rp.lk (mmap stable while open) */
    static _Thread_local uint8_t *raw;
    static _Thread_local uint32_t raw_cap;
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open || i < 0 || i >= g_rp.y10.n) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    uint32_t plen;
    const uint8_t *p = rchan_payload(&g_rp.y10, i, &plen, NULL);
    int w = g_rp.nat_w, h = g_rp.nat_h, mode = g_rp.nat_mode;
    int median = ev_int(ev_at(EV_EO, g_rp.y10.t_ms[i]), "median", 0);   /* as it was live */
    if (!p) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    if (plen > raw_cap) { free(raw); raw = malloc(plen); raw_cap = raw ? plen : 0; }
    if (!raw) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    memcpy(raw, p, plen);
    pthread_mutex_unlock(&g_rp.lk);

    pthread_mutex_lock(&g_nat.lk);
    if (g_nat.idx == i && g_nat.q == quality && g_nat.jpg) {   /* another thread just did it */
        memcpy(buf, g_nat.jpg, g_nat.len); *len = g_nat.len;
        pthread_mutex_unlock(&g_nat.lk);
        return 0;
    }
    int reseed = (i != g_nat.tone_i + 1);        /* EMA advances on sequential play */
    int rc = render_native_jpeg(raw, plen, w, h, mode, median, &g_nat.tone, reseed, quality,
                                buf, NAT_JPG_CAP, len);
    if (rc == 0) {
        g_nat.tone_i = i;
        if (!g_nat.jpg) g_nat.jpg = malloc(NAT_JPG_CAP);
        if (g_nat.jpg && *len <= NAT_JPG_CAP) {
            memcpy(g_nat.jpg, buf, *len); g_nat.len = *len; g_nat.idx = i; g_nat.q = quality;
        }
    }
    pthread_mutex_unlock(&g_nat.lk);
    return rc;
}

static void rp_unload(void)
{
    rchan_free(&g_rp.jpeg);
    rchan_free(&g_rp.radar);
    rchan_free(&g_rp.y10);
    rchan_free(&g_rp.det);
    nat_cache_drop();
    free(g_rp.evbuf);
    g_rp.evbuf = NULL;
    g_rp.n_evs = 0;
    g_rp.open = 0;
    g_rp.has_native = g_rp.has_display = g_rp.video_native = g_rp.has_illum = 0;
}

static int rp_open(const char *sid)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return -1;
    char dir[640], mf[24576], state[32] = "", name[NAME_MAX_LEN * 2] = "";
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);
    if (store_manifest_read(dir, mf, sizeof mf) < 0) return -1;
    store_manifest_field(mf, "state", state, sizeof state);
    if (!strcmp(state, "recording")) return -1;
    store_manifest_field(mf, "name", name, sizeof name);

    /* kick existing pushers off the old data */
    g_rp.gen++;
    pthread_cond_broadcast(&g_rp.cv);
    for (int i = 0; i < 400 && g_rp.pushers > 0; i++) {
        pthread_mutex_unlock(&g_rp.lk);
        usleep(5000);
        pthread_mutex_lock(&g_rp.lk);
    }
    rp_unload();

    /* every channel is optional — replay whatever the session really has
     * (a radar-only session replays scope + stats; video shows no frames) */
    uint64_t t0_rd = 0, t0_y10 = 0;
    uint64_t t0_det = 0;
    int has_jpeg = rchan_load(dir, "eo_jpeg", &g_rp.jpeg, &g_rp.t0_mono) == 0;
    int has_y10 = rchan_load(dir, "eo_y10", &g_rp.y10, &t0_y10) == 0;
    int has_radar = rchan_load(dir, "radar_wire", &g_rp.radar, &t0_rd) == 0;
    int has_det = rchan_load(dir, "det_wire", &g_rp.det, &t0_det) == 0;
    if (!has_jpeg && has_y10) g_rp.t0_mono = t0_y10;
    else if (!has_jpeg && has_radar) g_rp.t0_mono = t0_rd;
    else if (!has_jpeg && !has_y10 && !has_radar && has_det) g_rp.t0_mono = t0_det;
    if (!has_jpeg && !has_y10 && !has_radar && !has_det) return -1;   /* nothing to replay */
    load_events(dir);

    /* native geometry + encoding for reconstruction */
    g_rp.has_native = has_y10;
    g_rp.has_display = has_jpeg;
    g_rp.video_native = has_y10;                             /* default to native when present */
    g_rp.play_q = 55;                                        /* smooth over WiFi while playing */
    g_rp.play_fps = 20.0;                                    /* ~1.5-2.5 MB/s native during play */
    g_rp.nat_w = 1440; g_rp.nat_h = 1088; g_rp.nat_mode = MODE_Y10P;
    g_rp.tonemap_match = 1;
    if (has_y10) {
        char cj[512], v[24];
        char cpath[700];
        snprintf(cpath, sizeof cpath, "%s/eo_y10/channel.json", dir);
        FILE *cf = fopen(cpath, "rb");
        if (cf) {
            size_t n = fread(cj, 1, sizeof cj - 1, cf); cj[n] = 0; fclose(cf);
            if (store_manifest_field(cj, "w", v, sizeof v) == 0 && atoi(v) > 0) g_rp.nat_w = atoi(v);
            if (store_manifest_field(cj, "h", v, sizeof v) == 0 && atoi(v) > 0) g_rp.nat_h = atoi(v);
            if (store_manifest_field(cj, "encoding", v, sizeof v) == 0)
                g_rp.nat_mode = !strcmp(v, "y16le") ? MODE_RAW16 : !strcmp(v, "y8") ? MODE_Y8 : MODE_Y10P;
            /* does the current build's tone map match the one that recorded this? */
            if (store_manifest_field(cj, "tonemap_hash", v, sizeof v) == 0 && v[0])
                g_rp.tonemap_match = ((uint32_t)strtoul(v, NULL, 10) == eo_tonemap_hash());
            if (store_manifest_field(cj, "illum", v, sizeof v) == 0 && atoi(v) == 1)
                g_rp.has_illum = 1;
        }
    }
    nat_cache_drop();

    char v[64] = "";
    store_manifest_field(mf, "dur_ms", v, sizeof v);
    g_rp.dur_ms = atoll(v);
    if (g_rp.dur_ms <= 0 && g_rp.jpeg.n)
        g_rp.dur_ms = g_rp.jpeg.t_ms[g_rp.jpeg.n - 1];
    if (g_rp.dur_ms <= 0 && g_rp.y10.n)
        g_rp.dur_ms = g_rp.y10.t_ms[g_rp.y10.n - 1];
    if (g_rp.dur_ms <= 0 && g_rp.radar.n)
        g_rp.dur_ms = g_rp.radar.t_ms[g_rp.radar.n - 1];
    if (g_rp.dur_ms <= 0 && g_rp.det.n)
        g_rp.dur_ms = g_rp.det.t_ms[g_rp.det.n - 1];
    const char *rt = strstr(mf, "\"t_start\"");
    g_rp.t0_real = 0;
    if (rt) {
        const char *r2 = strstr(rt, "\"realtime_ns\":");
        if (r2) g_rp.t0_real = strtoull(r2 + 14, NULL, 10);
    }
    tonemap_verify_vs_display();                 /* ~10ms: does my tone map still match EO? */
    if (g_rp.tm_vs_eo < 0) g_rp.tonemap_match = 0;   /* drift vs the live feed */

    snprintf(g_rp.sid, sizeof g_rp.sid, "%s", sid);
    snprintf(g_rp.name, sizeof g_rp.name, "%s", name);
    /* Do NOT transcode on open. The console defaults replays to the display view;
     * a full libx264 encode (~2 cores) per open — that close() never killed —
     * stacked up and pegged the box, freezing the live camera. The native mp4 is
     * built only when explicitly requested (the /replay/native.mp4 or
     * /replay/transcode handler) or pre-built at save. A build is NOT cancelled by
     * open/close, so a "Convert to native" started from the library runs to
     * completion in the background; transcode_request caps it to one at a time. */
    g_rp.open = 1;
    g_rp.rate = 1.0;
    clock_set(0, 0);
    return 0;
}

void replay_close(void)
{
    /* Deliberately do NOT cancel the native encode here — a "Convert to native"
     * must survive leaving the movie screen. Only one ever runs (transcode_request
     * caps to 1), so it cannot stack/peg the box. */
    pthread_mutex_lock(&g_rp.lk);
    g_rp.gen++;
    pthread_cond_broadcast(&g_rp.cv);
    for (int i = 0; i < 400 && g_rp.pushers > 0; i++) {
        pthread_mutex_unlock(&g_rp.lk);
        usleep(5000);
        pthread_mutex_lock(&g_rp.lk);
    }
    rp_unload();
    pthread_mutex_unlock(&g_rp.lk);
}

/* ---- ctl ---- */

void replay_ctl(const char *qs, char *resp, size_t rlen)
{
    char v[64];
    pthread_mutex_lock(&g_rp.lk);

    if (query_get(qs, "open", v, sizeof v) == 0) {
        int r = rp_open(v);
        snprintf(resp, rlen, r == 0 ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"open failed\"}");
        pthread_mutex_unlock(&g_rp.lk);
        return;
    }
    if (query_get(qs, "close", v, sizeof v) == 0) {
        pthread_mutex_unlock(&g_rp.lk);
        replay_close();
        snprintf(resp, rlen, "{\"ok\":1}");
        return;
    }
    if (!g_rp.open) {
        snprintf(resp, rlen, "{\"ok\":0,\"err\":\"no session open\"}");
        pthread_mutex_unlock(&g_rp.lk);
        return;
    }
    if (query_get(qs, "rate", v, sizeof v) == 0) {        /* composable with play/seek */
        double r = atof(v);
        int64_t t = clock_now();
        if (r >= 0.1 && r <= 16.0) g_rp.rate = r;
        clock_set(t, g_rp.playing);
    }
    if (query_get(qs, "play", v, sizeof v) == 0) {
        int64_t t = clock_now();
        clock_set(t >= g_rp.dur_ms ? 0 : t, 1);           /* replay from start at end */
    } else if (query_get(qs, "pause", v, sizeof v) == 0) {
        clock_set(clock_now(), 0);
    } else if (query_get(qs, "seek", v, sizeof v) == 0) {
        clock_set(atoll(v), g_rp.playing);
    } else if (query_get(qs, "video", v, sizeof v) == 0) {
        int want_nat = !strcmp(v, "native");
        g_rp.video_native = want_nat && g_rp.has_native;
        pthread_cond_broadcast(&g_rp.cv);                 /* pushers re-pick source */
    } else if (query_get(qs, "playq", v, sizeof v) == 0) {
        int q = atoi(v);
        if (q >= 20 && q <= 95) g_rp.play_q = q;          /* native play bandwidth/quality */
        pthread_cond_broadcast(&g_rp.cv);
    } else if (query_get(qs, "playfps", v, sizeof v) == 0) {
        double f = atof(v);
        if (f >= 2.0 && f <= 60.0) g_rp.play_fps = f;
        pthread_cond_broadcast(&g_rp.cv);
    } else if (query_get(qs, "step", v, sizeof v) == 0) {
        /* step over the active video channel, else radar frames */
        RChan *vc = video_chan();
        const RChan *c = vc->n ? vc : &g_rp.radar;
        if (c->n) {
            int dir = atoi(v) < 0 ? -1 : 1;
            long j = rchan_at(c, clock_now()) + dir;
            if (j < 0) j = 0;
            if (j >= c->n) j = c->n - 1;
            clock_set(c->t_ms[j], 0);
        }
    }
    snprintf(resp, rlen, "{\"ok\":1}");
    pthread_mutex_unlock(&g_rp.lk);
}

/* ---- JSON views ---- */

static size_t rp_state_obj(char *buf, size_t len, int64_t t)
{
    RChan *vc = video_chan();
    long fi = rchan_at(vc, t);
    int mp4_pct = 0;
    int mp4_st = g_rp.has_native ? transcode_status(g_rp.sid, &mp4_pct) : 0;
    const char *mp4_state = mp4_st == 2 ? "ready" : mp4_st == 1 ? "building"
                          : mp4_st < 0 ? "failed" : "none";

    /* per-frame illuminator (from the native frame's meta[4] at this instant) */
    char illum[80] = "";
    if (g_rp.has_illum && g_rp.y10.n) {
        long yi = rchan_at(&g_rp.y10, t);
        if (yi < 0) yi = 0;
        const AirecRecHdr *h;
        uint32_t l;
        if (rchan_payload(&g_rp.y10, yi, &l, &h) && h) {
            uint32_t im = h->meta[EO_META_ILLUM];
            snprintf(illum, sizeof illum,
                ",\"illum\":{\"on\":%d,\"power\":%d,\"fov\":%.1f,\"present\":%d}",
                ILLUM_ON(im), ILLUM_POWER(im), ILLUM_FOV_X10(im) / 10.0, ILLUM_PRESENT(im));
        }
    }
    return (size_t)snprintf(buf, len,
        "{\"sid\":\"%s\",\"name\":\"%s\",\"t_ms\":%lld,\"dur_ms\":%lld,"
        "\"playing\":%d,\"rate\":%.2f,\"t_wall_ms\":%llu,\"frame_i\":%ld,\"frames\":%ld,"
        "\"video_src\":\"%s\",\"has_native\":%d,\"has_display\":%d,\"native_w\":%d,\"native_h\":%d,"
        "\"play_q\":%d,\"play_fps\":%.0f,\"tonemap_match\":%d,"
        "\"tonemap_vs_eo\":\"%s\",\"tonemap_vs_eo_diff\":%.1f,"
        "\"native_mp4\":\"%s\",\"native_mp4_pct\":%d%s}",
        g_rp.sid, g_rp.name, (long long)t, (long long)g_rp.dur_ms,
        g_rp.playing, g_rp.rate,
        (unsigned long long)((g_rp.t0_real + (uint64_t)t * 1000000ull) / 1000000ull),
        fi, vc->n,
        g_rp.video_native && g_rp.has_native ? "native" : "display",
        g_rp.has_native, g_rp.has_display, g_rp.nat_w, g_rp.nat_h,
        g_rp.play_q, g_rp.play_fps, g_rp.tonemap_match,
        g_rp.tm_vs_eo == 1 ? "ok" : g_rp.tm_vs_eo < 0 ? "drift" : "unchecked",
        g_rp.tm_vs_eo_diff, mp4_state, mp4_pct, illum);
}

void replay_state_json(char *buf, size_t len)
{
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open) {
        snprintf(buf, len, "{\"open\":false,\"replay\":true}");
    } else {
        int64_t t = clock_now();
        size_t o = (size_t)snprintf(buf, len, "{\"open\":true,\"replay\":true,\"state\":");
        o += rp_state_obj(buf + o, len - o, t);
        snprintf(buf + o, len - o, "}");
    }
    pthread_mutex_unlock(&g_rp.lk);
}

void replay_stats_json(char *buf, size_t len)
{
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open) {
        snprintf(buf, len, "{\"open\":false,\"replay\":true}");
        pthread_mutex_unlock(&g_rp.lk);
        return;
    }
    int64_t t = clock_now();
    const Ev *eo = ev_at(EV_EO, t), *app = ev_at(EV_APP, t);
    size_t o = (size_t)snprintf(buf, len, "{\"replay\":true,\"replay_state\":");
    o += rp_state_obj(buf + o, len - o, t);
    o += (size_t)snprintf(buf + o, len - o, ",\"eo\":");
    if (eo && o + (size_t)eo->len + 64 < len) { memcpy(buf + o, eo->body, (size_t)eo->len); o += (size_t)eo->len; }
    else o += (size_t)snprintf(buf + o, len - o, "null");
    o += (size_t)snprintf(buf + o, len - o, ",\"app\":");
    if (app && o + (size_t)app->len + 8 < len) { memcpy(buf + o, app->body, (size_t)app->len); o += (size_t)app->len; }
    else o += (size_t)snprintf(buf + o, len - o, "null");
    snprintf(buf + o, len - o, "}");
    pthread_mutex_unlock(&g_rp.lk);
}

int replay_radar_json(char *buf, size_t len)
{
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    int64_t t = clock_now();
    long i = rchan_at(&g_rp.radar, t);
    if (i < 0) i = g_rp.radar.n ? 0 : -1;   /* before first frame: hold the first */
    if (i < 0) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    uint32_t plen;
    const uint8_t *p = rchan_payload(&g_rp.radar, i, &plen, NULL);
    if (!p || plen < 2 || p[0] != '{' || plen + 20 > len) {
        pthread_mutex_unlock(&g_rp.lk);
        return -1;
    }
    /* inject "replay":true, keep the recorded frame byte-verbatim otherwise */
    size_t o = (size_t)snprintf(buf, len, "{\"replay\":true,");
    memcpy(buf + o, p + 1, plen - 1);
    buf[o + plen - 1] = 0;
    pthread_mutex_unlock(&g_rp.lk);
    return 0;
}

/* recorded EO detections at <= clock, byte-verbatim (same shape as live det). */
int replay_det_json(char *buf, size_t len)
{
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    int64_t t = clock_now();
    long i = rchan_at(&g_rp.det, t);
    if (i < 0) i = g_rp.det.n ? 0 : -1;
    if (i < 0) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    uint32_t plen;
    const uint8_t *p = rchan_payload(&g_rp.det, i, &plen, NULL);
    if (!p || plen < 2 || p[0] != '{' || plen + 20 > len) {
        pthread_mutex_unlock(&g_rp.lk);
        return -1;
    }
    size_t o = (size_t)snprintf(buf, len, "{\"replay\":true,");
    memcpy(buf + o, p + 1, plen - 1);
    buf[o + plen - 1] = 0;
    pthread_mutex_unlock(&g_rp.lk);
    return 0;
}

int replay_rstats_json(char *buf, size_t len)
{
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    const Ev *rd = ev_at(EV_RD, clock_now());
    if (!rd || (size_t)rd->len + 20 > len || rd->body[0] != '{') {
        pthread_mutex_unlock(&g_rp.lk);
        return -1;
    }
    size_t o = (size_t)snprintf(buf, len, "{\"replay\":true,");
    memcpy(buf + o, rd->body + 1, (size_t)rd->len - 1);
    buf[o + (size_t)rd->len - 1] = 0;
    pthread_mutex_unlock(&g_rp.lk);
    return 0;
}

int replay_frame_copy(int64_t t_ms, uint8_t *buf, uint32_t cap, uint32_t *len)
{
    pthread_mutex_lock(&g_rp.lk);
    if (!g_rp.open) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    int native = g_rp.video_native && g_rp.has_native;
    RChan *vc = video_chan();
    if (!vc->n) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    long i = rchan_at(vc, t_ms);
    if (i < 0) i = 0;
    if (native) {
        pthread_mutex_unlock(&g_rp.lk);      /* nat_jpeg takes g_rp.lk itself */
        if (cap < NAT_JPG_CAP) return -1;
        return nat_jpeg(i, NAT_FULL_Q, buf, len);   /* single-frame inspect: full detail */
    }
    uint32_t plen;
    const uint8_t *p = rchan_payload(vc, i, &plen, NULL);
    if (!p || plen > cap) { pthread_mutex_unlock(&g_rp.lk); return -1; }
    memcpy(buf, p, plen);                /* copy under lock: mmap can't vanish */
    *len = plen;
    pthread_mutex_unlock(&g_rp.lk);
    return 0;
}

/* ---- MJPEG pusher ---- */

void replay_stream(int fd)
{
    static const char *head =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (write(fd, head, strlen(head)) < 0) return;

    uint8_t *natbuf = malloc(NAT_JPG_CAP);   /* per-connection native scratch */

    pthread_mutex_lock(&g_rp.lk);
    unsigned gen = g_rp.gen;
    g_rp.pushers++;
    long last = -2;
    uint64_t last_emit = 0;

    while (g_rp.open && g_rp.gen == gen) {
        int64_t t = clock_now();
        int native = g_rp.video_native && g_rp.has_native;
        RChan *vc = video_chan();
        long i = rchan_at(vc, t);
        if (i < 0) i = vc->n ? 0 : -1;

        /* Bandwidth cap: native frames are big; while PLAYING, hold output to
         * play_fps at play_q so a thin WiFi link stays smooth. Paused/stepped/
         * scrubbed frames are always full-res full-quality (single frame, not a
         * bandwidth problem). Display-source frames are already small: no cap. */
        double fps_cap = (native && g_rp.playing) ? g_rp.play_fps : 0.0;
        uint64_t nowns = now_ns();
        int throttled = fps_cap > 0.0 && nowns < last_emit + (uint64_t)(1e9 / fps_cap);
        int quality = g_rp.playing ? g_rp.play_q : NAT_FULL_Q;

        if (i >= 0 && i != last && !throttled) {
            last = i;
            last_emit = nowns;
            int bad = 0;
            if (native && natbuf) {
                pthread_mutex_unlock(&g_rp.lk);
                uint32_t jlen = 0;
                if (nat_jpeg(i, quality, natbuf, &jlen) == 0) {   /* decode+tonemap+encode, cached */
                    char ph[128];
                    int hn = snprintf(ph, sizeof ph,
                        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", jlen);
                    bad = write(fd, ph, (size_t)hn) != hn ||
                          write(fd, natbuf, jlen) != (ssize_t)jlen ||
                          write(fd, "\r\n", 2) != 2;
                }
                pthread_mutex_lock(&g_rp.lk);
            } else {
                uint32_t plen;
                const uint8_t *p = rchan_payload(vc, i, &plen, NULL);
                /* Copy the frame OUT of the mmap while holding the lock, then write
                 * from the heap. Writing the mmap pointer after unlocking races a
                 * concurrent replay_close() that unmaps after a bounded drain — a
                 * stalled socket would otherwise read freed/unmapped memory (SIGSEGV).
                 * (natbuf is 1 MiB; display/wire frames are <=512 KiB.) */
                if (p && natbuf && plen <= NAT_JPG_CAP) {
                    memcpy(natbuf, p, plen);
                    pthread_mutex_unlock(&g_rp.lk);
                    char ph[128];
                    int hn = snprintf(ph, sizeof ph,
                        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", plen);
                    bad = write(fd, ph, (size_t)hn) != hn ||
                          write(fd, natbuf, plen) != (ssize_t)plen ||
                          write(fd, "\r\n", 2) != 2;
                    pthread_mutex_lock(&g_rp.lk);
                }
            }
            if (bad) break;
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_rp.cv, &g_rp.lk, &ts);
    }
    g_rp.pushers--;
    pthread_mutex_unlock(&g_rp.lk);
    free(natbuf);
}

/* Generic replay SSE pusher — the replay twin of the live `/radar/stream` and
 * `/det/stream`. Live radar/det are SSE-pushed at the sensor rate; a fixed poll
 * (120/150 ms) undersamples and lags. Emit each recorded frame as the playback
 * clock crosses it (the 5 ms wait is well under frame spacing at 1x, so nothing
 * is dropped; higher rates coalesce). Wire JSON byte-verbatim + "replay":true,
 * paced by the shared clock/cv, gen-gated to drop on open/close/source-switch.
 *
 * The frame is COPIED into a per-connection heap buffer under the lock and written
 * from the heap: writing the mmap pointer after unlocking races a concurrent
 * replay_close() that unmaps after a bounded drain — a stalled socket would
 * otherwise read freed/unmapped memory (SIGSEGV). */
static void replay_sse_stream(int fd, RChan *chan)
{
    static const char *head =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (write(fd, head, strlen(head)) < 0) return;
    char *sbuf = malloc(NAT_JPG_CAP);            /* holds the whole SSE frame (1 MiB >= 256 KiB wire cap) */
    if (!sbuf) return;

    pthread_mutex_lock(&g_rp.lk);
    unsigned gen = g_rp.gen;
    g_rp.pushers++;
    long last = -2;

    while (g_rp.open && g_rp.gen == gen) {
        int64_t t = clock_now();
        long i = rchan_at(chan, t);
        if (i < 0) i = chan->n ? 0 : -1;                  /* before first: hold the first */
        if (i >= 0 && i != last) {
            last = i;
            uint32_t plen;
            const uint8_t *p = rchan_payload(chan, i, &plen, NULL);
            if (p && plen >= 2 && p[0] == '{' && (size_t)plen + 24 <= NAT_JPG_CAP) {
                size_t o = 0;
                memcpy(sbuf, "data: {\"replay\":true", 20); o = 20;
                if (plen > 2) sbuf[o++] = ',';            /* non-empty keeps the comma; "{}" stays valid JSON */
                memcpy(sbuf + o, p + 1, plen - 1); o += plen - 1;   /* rest incl closing } */
                sbuf[o++] = '\n'; sbuf[o++] = '\n';
                pthread_mutex_unlock(&g_rp.lk);
                int bad = write(fd, sbuf, o) != (ssize_t)o;
                pthread_mutex_lock(&g_rp.lk);
                if (bad) break;
            }
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_rp.cv, &g_rp.lk, &ts);
    }
    g_rp.pushers--;
    pthread_mutex_unlock(&g_rp.lk);
    free(sbuf);
}

void replay_radar_stream(int fd) { replay_sse_stream(fd, &g_rp.radar); }
void replay_det_stream(int fd)   { replay_sse_stream(fd, &g_rp.det); }
