/* session.c — recording lifecycle: recording -> pending -> saved|discarded,
 * crash recovery to `recovered`, disk guard, manifest authoring.
 *
 * Manifest layout is fixed: a MUTABLE prefix (state/name/tags/note/ai) followed
 * by an immutable tail starting at "t_start" — save/annotate rewrites the
 * prefix and preserves the tail verbatim.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

Rec g_rec = { .state = ST_IDLE, .mode = MODE_Y10P, .keep = 1,
              .lk = PTHREAD_MUTEX_INITIALIZER,
              .root = REC_DEF_ROOT, .port = REC_DEF_PORT };

#define START_FLOOR_GB 20.0
#define STOP_FLOOR_GB   5.0

static char g_cfg_eo[8192], g_cfg_rd[8192];   /* config_at_start snapshots */
static uint64_t g_stop_mono, g_stop_real;
static char g_stop_reason[24] = "operator";

/* ---- JSON helpers ---- */

void json_escape(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 4 < outlen; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, outlen - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void tags_csv_to_json(const char *csv, char *out, size_t outlen)
{
    size_t o = 0;
    out[o++] = '[';
    const char *p = csv;
    int first = 1;
    while (p && *p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && o + n + 4 < outlen) {
            if (!first) out[o++] = ',';
            out[o++] = '"';
            for (size_t i = 0; i < n; i++)
                if (p[i] != '"' && p[i] != '\\') out[o++] = p[i];
            out[o++] = '"';
            first = 0;
        }
        p = e ? e + 1 : NULL;
    }
    out[o++] = ']';
    out[o] = 0;
}

static void iso8601_utc(uint64_t real_ns_v, char *out, size_t outlen)
{
    time_t s = (time_t)(real_ns_v / 1000000000ull);
    struct tm tm;
    gmtime_r(&s, &tm);
    strftime(out, outlen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ---- manifest ---- */

static void manifest_write(const char *dir, const char *sid, const char *state,
                           const char *name, const char *tags_json, const char *note,
                           uint64_t t0m, uint64_t t0r,
                           uint64_t tsm, uint64_t tsr, const char *reason)
{
    char iso0[40] = "", iso1[40] = "", en[NAME_MAX_LEN * 2], eno[NOTE_MAX_LEN * 2];
    iso8601_utc(t0r, iso0, sizeof iso0);
    if (tsr) iso8601_utc(tsr, iso1, sizeof iso1);
    json_escape(name, en, sizeof en);
    json_escape(note, eno, sizeof eno);

    uint64_t recs = 0, bytes = 0, drops = 0, werr = 0;
    char chans[2048];
    size_t co = 0;
    co += (size_t)snprintf(chans + co, sizeof chans - co, "[");
    for (int i = 0; i < CH_N; i++) {
        Chan *c = &g_chan[i];
        co += (size_t)snprintf(chans + co, sizeof chans - co,
            "%s{\"name\":\"%s\",\"records\":%llu,\"bytes\":%llu,\"drops\":%llu,"
            "\"write_errors\":%llu,\"bad_size\":%llu,\"tap_reattach\":%llu}",
            i ? "," : "", c->cfg->name,
            (unsigned long long)c->records, (unsigned long long)c->bytes,
            (unsigned long long)(c->drops_ring + c->drops_queue),
            (unsigned long long)c->write_errors, (unsigned long long)c->bad_size,
            (unsigned long long)c->tap_reattach);
        recs += c->records; bytes += c->bytes; drops += c->drops_ring + c->drops_queue;
        werr += c->write_errors;
    }
    snprintf(chans + co, sizeof chans - co, "]");

    char buf[24576];
    int n = snprintf(buf, sizeof buf,
        "{\"airec\":1,\"sid\":\"%s\",\n"
        "\"state\":\"%s\",\"name\":\"%s\",\"tags\":%s,\"note\":\"%s\",\n"
        "\"ai\":{\"state\":\"none\",\"note\":\"\",\"tags\":[],\"model\":\"\",\"ts\":0},\n"
        "\"t_start\":{\"mono_ns\":%llu,\"realtime_ns\":%llu,\"iso8601\":\"%s\"},\n"
        "\"t_stop\":{\"mono_ns\":%llu,\"realtime_ns\":%llu,\"iso8601\":\"%s\"},\n"
        "\"dur_ms\":%llu,\"stopped_reason\":\"%s\",\n"
        "\"mode\":\"%s\",\"keep\":%d,\n"
        "\"totals\":{\"bytes\":%llu,\"records\":%llu,\"drops\":%llu},\n"
        "\"damaged\":%d,\n"          /* a disk write failed: this recording has holes */
        "\"channels\":%s,\n"
        "\"config_at_start\":{\"eo_stats\":%s,\"radar_stats\":%s}}\n",
        sid, state, en, tags_json, eno,
        (unsigned long long)t0m, (unsigned long long)t0r, iso0,
        (unsigned long long)tsm, (unsigned long long)tsr, iso1,
        (unsigned long long)(tsm > t0m ? (tsm - t0m) / 1000000ull : 0), reason,
        g_rec.mode == MODE_Y10P ? "y10p" : g_rec.mode == MODE_Y8 ? "y8" : "raw16",
        g_rec.keep,
        (unsigned long long)bytes, (unsigned long long)recs, (unsigned long long)drops,
        werr != 0,
        chans,
        g_cfg_eo[0] ? g_cfg_eo : "null", g_cfg_rd[0] ? g_cfg_rd : "null");

    char path[640];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    store_write_file_atomic(path, buf, (size_t)(n > 0 && (size_t)n < sizeof buf ? n : 0));
}

/* rewrite only the mutable prefix; tail from "t_start" preserved verbatim */
static int manifest_edit_prefix(const char *dir, const char *state,
                                const char *name, const char *tags_csv, const char *note)
{
    char buf[24576];
    if (store_manifest_read(dir, buf, sizeof buf) < 0) return -1;
    const char *tail = strstr(buf, "\"t_start\"");
    if (!tail) return -1;

    char sid[64] = "", curname[NAME_MAX_LEN * 2] = "", curtags[TAGS_MAX_LEN * 2] = "[]",
         curnote[NOTE_MAX_LEN * 2] = "";
    store_manifest_field(buf, "sid", sid, sizeof sid);
    store_manifest_field(buf, "name", curname, sizeof curname);
    store_manifest_field(buf, "tags", curtags, sizeof curtags);
    store_manifest_field(buf, "note", curnote, sizeof curnote);

    char en[NAME_MAX_LEN * 2], eno[NOTE_MAX_LEN * 2], tj[TAGS_MAX_LEN * 2];
    json_escape(name && name[0] ? name : curname, en, sizeof en);
    json_escape(note ? note : curnote, eno, sizeof eno);
    if (tags_csv) tags_csv_to_json(tags_csv, tj, sizeof tj);
    else snprintf(tj, sizeof tj, "%s", curtags[0] ? curtags : "[]");

    char out[24576];
    int n = snprintf(out, sizeof out,
        "{\"airec\":1,\"sid\":\"%s\",\n"
        "\"state\":\"%s\",\"name\":\"%s\",\"tags\":%s,\"note\":\"%s\",\n"
        "\"ai\":{\"state\":\"none\",\"note\":\"\",\"tags\":[],\"model\":\"\",\"ts\":0},\n"
        "%s", sid, state, en, tj, eno, tail);
    if (n < 0 || (size_t)n >= sizeof out) return -1;

    char path[640];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    store_write_file_atomic(path, out, (size_t)n);
    return 0;
}

/* ---- events helpers ---- */

static void submit_event(const char *type, const char *body_json)
{
    char ev[16384];
    int n = snprintf(ev, sizeof ev, "{\"type\":\"%s\",\"t_mono_ns\":%llu,\"body\":%s}",
                     type, (unsigned long long)now_ns(), body_json ? body_json : "null");
    if (n > 0 && (size_t)n < sizeof ev) chan_submit(CH_EVENTS, ev, (uint32_t)n, now_ns(), NULL);
}

void session_clock_anchor(void)
{
    char b[128];
    snprintf(b, sizeof b, "{\"mono_ns\":%llu,\"realtime_ns\":%llu}",
             (unsigned long long)now_ns(), (unsigned long long)real_ns());
    submit_event("clock_anchor", b);
}

void session_marker(const char *text)
{
    char esc[512], b[600];
    json_escape(text, esc, sizeof esc);
    snprintf(b, sizeof b, "{\"text\":\"%s\"}", esc);
    submit_event("marker", b);
}

/* ---- lifecycle ---- */

int session_start(char *err, size_t errlen)
{
    pthread_mutex_lock(&g_rec.lk);
    if (g_rec.state != ST_IDLE) {
        snprintf(err, errlen, "already recording");
        pthread_mutex_unlock(&g_rec.lk);
        return -1;
    }
    if (!disk_present(g_rec.root)) {
        snprintf(err, errlen, "data volume absent");
        pthread_mutex_unlock(&g_rec.lk);
        return -1;
    }
    if (disk_free_gb(g_rec.root) < START_FLOOR_GB) {
        snprintf(err, errlen, "disk below %.0f GB", START_FLOOR_GB);
        pthread_mutex_unlock(&g_rec.lk);
        return -1;
    }

    /* Recording owns the machine. Kill any HD encode in flight -- it is a
     * software H.264 build (no encoder silicon on this SoC) and must never
     * compete with the live pipeline for CPU or memory bandwidth. The build
     * aborts on its next frame; the catch-up worker rebuilds it once idle. */
    transcode_cancel();

    time_t s = time(NULL);
    struct tm tm;
    gmtime_r(&s, &tm);
    strftime(g_rec.sid, sizeof g_rec.sid, "%Y%m%dT%H%M%SZ", &tm);
    snprintf(g_rec.dir, sizeof g_rec.dir, "%s/%s", g_rec.root, g_rec.sid);
    if (mkdir(g_rec.dir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errlen, "mkdir failed: %s", strerror(errno));
        pthread_mutex_unlock(&g_rec.lk);
        return -1;
    }

    g_cfg_eo[0] = g_cfg_rd[0] = 0;
    http_get_local(REC_EO_PORT, "/stats", g_cfg_eo, sizeof g_cfg_eo);
    http_get_local(REC_RD_PORT, "/stats", g_cfg_rd, sizeof g_cfg_rd);

    g_rec.t0_mono_ns = now_ns();
    g_rec.t0_real_ns = real_ns();
    g_stop_mono = g_stop_real = 0;
    snprintf(g_stop_reason, sizeof g_stop_reason, "operator");

    manifest_write(g_rec.dir, g_rec.sid, "recording", g_rec.sid, "[]", "",
                   g_rec.t0_mono_ns, g_rec.t0_real_ns, 0, 0, "operator");

    int fail = 0;
    for (int i = 0; i < CH_N; i++)
        if (chan_session_open(&g_chan[i], g_rec.dir, g_rec.t0_mono_ns) != 0) fail = 1;
    if (fail) {
        for (int i = 0; i < CH_N; i++) chan_session_close(&g_chan[i]);
        snprintf(err, errlen, "channel open failed");
        pthread_mutex_unlock(&g_rec.lk);
        return -1;
    }

    g_rec.state = ST_RECORDING;
    pthread_mutex_unlock(&g_rec.lk);

    session_clock_anchor();
    if (g_cfg_eo[0]) submit_event("eo_stats", g_cfg_eo);
    if (g_cfg_rd[0]) submit_event("radar_stats", g_cfg_rd);
    return 0;
}

int session_stop(const char *reason)
{
    pthread_mutex_lock(&g_rec.lk);
    if (g_rec.state != ST_RECORDING) {
        pthread_mutex_unlock(&g_rec.lk);
        return -1;
    }
    snprintf(g_stop_reason, sizeof g_stop_reason, "%s", reason);
    session_clock_anchor();
    g_stop_mono = now_ns();
    g_stop_real = real_ns();

    for (int i = 0; i < CH_N; i++) chan_session_close(&g_chan[i]);

    manifest_write(g_rec.dir, g_rec.sid, "pending", g_rec.sid, "[]", "",
                   g_rec.t0_mono_ns, g_rec.t0_real_ns, g_stop_mono, g_stop_real, g_stop_reason);

    snprintf(g_rec.pending_sid, sizeof g_rec.pending_sid, "%s", g_rec.sid);
    g_rec.state = ST_IDLE;
    pthread_mutex_unlock(&g_rec.lk);
    return 0;
}

static int sid_dir(const char *sid, char *dir, size_t dirlen)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/') || strchr(sid, '.')) return -1;
    snprintf(dir, dirlen, "%s/%s", g_rec.root, sid);
    char probe[700];
    snprintf(probe, sizeof probe, "%s/manifest.json", dir);
    return access(probe, F_OK) == 0 ? 0 : -1;
}

int session_save(const char *sid, const char *name, const char *tags, const char *note)
{
    char dir[640];
    if (sid_dir(sid, dir, sizeof dir) != 0) return -1;
    if (manifest_edit_prefix(dir, "saved", name, tags, note) != 0) return -1;
    thumbs_generate(dir);
    /* Pre-build native.mp4 now (async, nice/ionice-lowered — same as at replay
     * open, just earlier) so opening a native replay is instant and never fires a
     * transcode CPU spike while an operator is live. No-op if the session has no
     * native channel. */
    transcode_request(sid);
    pthread_mutex_lock(&g_rec.lk);                 /* pending_sid is also written by session_stop under this lock */
    if (!strcmp(sid, g_rec.pending_sid)) g_rec.pending_sid[0] = 0;
    pthread_mutex_unlock(&g_rec.lk);
    return 0;
}

int session_discard(const char *sid)
{
    char dir[640];
    if (sid_dir(sid, dir, sizeof dir) != 0) return -1;
    store_manifest_set_state(dir, "discarded");
    store_rm_rf_async(dir);
    pthread_mutex_lock(&g_rec.lk);
    if (!strcmp(sid, g_rec.pending_sid)) g_rec.pending_sid[0] = 0;
    pthread_mutex_unlock(&g_rec.lk);
    return 0;
}

int session_delete(const char *sids_csv)
{
    char csv[1024];
    snprintf(csv, sizeof csv, "%s", sids_csv);
    int n_ok = 0;
    char *save = NULL;
    for (char *tok = strtok_r(csv, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char dir[640];
        if (sid_dir(tok, dir, sizeof dir) == 0) { store_rm_rf_async(dir); n_ok++; }
    }
    return n_ok;
}

int session_purge_native(const char *sid)
{
    char dir[640];
    if (sid_dir(sid, dir, sizeof dir) != 0) return -1;
    char nat[700];
    snprintf(nat, sizeof nat, "%s/eo_y10", dir);
    if (access(nat, F_OK) != 0) return -1;
    store_rm_rf_async(nat);
    return 0;
}

/* ---- crash recovery ---- */

static uint64_t recover_channel(const char *dir, const ChanCfg *cfg)
{
    char ipath[700];
    snprintf(ipath, sizeof ipath, "%s/%s/index.bin", dir, cfg->name);
    FILE *f = fopen(ipath, "r+b");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long isz = ftell(f);
    long rows = isz / (long)sizeof(AirecIdxRow);
    long good = 0;
    AirecIdxRow row;

    for (long k = rows - 1; k >= 0; k--) {
        fseek(f, k * (long)sizeof row, SEEK_SET);
        if (fread(&row, sizeof row, 1, f) != 1) continue;
        char spath[700];
        snprintf(spath, sizeof spath, "%s/%s/data.%05u.airec", dir, cfg->name, row.segment_no);
        FILE *sf = fopen(spath, "rb");
        if (!sf) continue;
        AirecRecHdr h;
        int ok = fseek(sf, (long)row.offset, SEEK_SET) == 0 &&
                 fread(&h, sizeof h, 1, sf) == 1 &&
                 h.magic == AIREC_REC_MAGIC && h.seq == row.seq &&
                 h.payload_len == row.payload_len;
        if (ok) {
            /* verify the payload is fully on disk (fallocate'd zeros fail crc) */
            uint8_t *pl = malloc(h.payload_len);
            ok = pl && fread(pl, 1, h.payload_len, sf) == h.payload_len &&
                 crc32c(0, pl, h.payload_len) == h.crc32c;
            free(pl);
        }
        if (ok) {
            good = k + 1;
            uint64_t end = row.offset + sizeof h + ((row.payload_len + 7u) & ~7u);
            fclose(sf);
            if (truncate(spath, (off_t)end) != 0) {}
            /* drop any segments after this one */
            for (uint32_t s = row.segment_no + 1; ; s++) {
                snprintf(spath, sizeof spath, "%s/%s/data.%05u.airec", dir, cfg->name, s);
                if (unlink(spath) != 0) break;
            }
            break;
        }
        fclose(sf);
    }
    if (ftruncate(fileno(f), good * (long)sizeof row) != 0) {}
    fclose(f);
    return (uint64_t)good;
}

void session_recover_all(void)
{
    /* sweep interrupted deletes */
    DIR *d = opendir(g_rec.root);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, ".deleting-", 10)) {
                char p[640];
                snprintf(p, sizeof p, "%s/%s", g_rec.root, e->d_name);
                store_rm_rf_async(p);
            }
        closedir(d);
    }

    char sids[256][SID_LEN + 1];
    int n = store_list_sids(g_rec.root, sids, 256);
    uint64_t now_real = real_ns();

    for (int i = 0; i < n; i++) {
        char dir[640], buf[24576], state[32] = "", iso[64] = "";
        snprintf(dir, sizeof dir, "%s/%.16s", g_rec.root, sids[i]);
        if (store_manifest_read(dir, buf, sizeof buf) < 0) {
            store_rm_rf_async(dir);                      /* dir without manifest */
            continue;
        }
        store_manifest_field(buf, "state", state, sizeof state);

        if (!strcmp(state, "recording")) {
            fprintf(stderr, "rec: recovering crashed session %s\n", sids[i]);
            for (int c = 0; c < CH_N; c++) recover_channel(dir, &g_chan_cfg[c]);
            store_manifest_set_state(dir, "recovered");
            store_manifest_field(buf, "state", state, sizeof state);
            snprintf(state, sizeof state, "recovered");
        }
        if (!strcmp(state, "discarded")) { store_rm_rf_async(dir); continue; }

        /* pending/recovered sessions older than 24 h auto-purge */
        if (!strcmp(state, "pending") || !strcmp(state, "recovered")) {
            struct tm tm; memset(&tm, 0, sizeof tm);
            if (sscanf(sids[i], "%4d%2d%2dT%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                       &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900; tm.tm_mon -= 1;
                time_t t = timegm(&tm);
                if (t > 0 && now_real / 1000000000ull > (uint64_t)t + 86400) {
                    fprintf(stderr, "rec: auto-purging stale pending %s\n", sids[i]);
                    store_rm_rf_async(dir);
                    continue;
                }
                if (!strcmp(state, "pending") || !strcmp(state, "recovered"))
                    snprintf(g_rec.pending_sid, sizeof g_rec.pending_sid, "%.16s", sids[i]);
            }
        }
        (void)iso;
    }
}

/* ---- disk guard + periodic anchors (thread) ---- */

static void *guard_thread(void *arg)
{
    (void)arg;
    uint64_t last_anchor = 0;
    for (;;) {
        sleep(2);
        if (g_rec.state != ST_RECORDING) continue;
        double free_gb = disk_free_gb(g_rec.root);
        if (free_gb > 0 && free_gb < STOP_FLOOR_GB) {
            fprintf(stderr, "rec: disk floor hit (%.1f GB) — auto-stop\n", free_gb);
            session_stop("disk_full");
            continue;
        }
        uint64_t t = now_ns();
        if (t - last_anchor > 30ull * 1000000000ull) {
            session_clock_anchor();
            last_anchor = t;
        }
    }
    return NULL;
}

void session_guard_start(void)
{
    static pthread_t t;
    pthread_create(&t, NULL, guard_thread, NULL);
}

/* ---- /stats ---- */

void session_stats_json(char *buf, size_t len)
{
    double freeg = disk_free_gb(g_rec.root), totg = disk_total_gb(g_rec.root);
    double mbs = 0;
    for (int i = 0; i < CH_N; i++) mbs += g_chan[i].mb_s;
    double est_min = g_rec.state == ST_RECORDING && mbs > 1
                   ? freeg * 1000.0 / mbs / 60.0
                   : freeg * 1000.0 / 130.0 / 60.0;

    size_t o = 0;
    o += (size_t)snprintf(buf + o, len - o,
        "{\"connected\":true,\"state\":\"%s\",\"disk_present\":%d,"
        "\"disk_free_gb\":%.1f,\"disk_total_gb\":%.1f,\"est_min_remaining\":%.0f,"
        "\"mode\":\"%s\",\"keep\":%d,",
        g_rec.state == ST_RECORDING ? "recording" : "idle",
        disk_present(g_rec.root), freeg, totg, est_min,
        g_rec.mode == MODE_Y10P ? "y10p" : g_rec.mode == MODE_Y8 ? "y8" : "raw16",
        g_rec.keep);
    if (g_rec.state == ST_RECORDING)
        o += (size_t)snprintf(buf + o, len - o,
            "\"rec_sid\":\"%s\",\"rec_elapsed_s\":%.1f,",
            g_rec.sid, (now_ns() - g_rec.t0_mono_ns) / 1e9);
    if (g_rec.pending_sid[0])
        o += (size_t)snprintf(buf + o, len - o, "\"pending_sid\":\"%s\",", g_rec.pending_sid);

    o += (size_t)snprintf(buf + o, len - o, "\"channels\":[");
    for (int i = 0; i < CH_N; i++) {
        Chan *c = &g_chan[i];
        o += (size_t)snprintf(buf + o, len - o,
            "%s{\"name\":\"%s\",\"connected\":%d,\"records\":%llu,\"bytes\":%llu,"
            "\"mb_s\":%.1f,\"drops_ring\":%llu,\"drops_queue\":%llu,\"lost\":%d,"
            "\"write_errors\":%llu,\"bad_size\":%llu,\"tap_reattach\":%llu}",
            i ? "," : "", c->cfg->name,
            c->cfg->tap ? c->sub_ok : 1,
            (unsigned long long)c->records, (unsigned long long)c->bytes, c->mb_s,
            (unsigned long long)c->drops_ring, (unsigned long long)c->drops_queue, c->lost,
            (unsigned long long)c->write_errors, (unsigned long long)c->bad_size,
            (unsigned long long)c->tap_reattach);
    }
    snprintf(buf + o, len - o, "]}\n");
}
