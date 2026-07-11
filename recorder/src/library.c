/* library.c — /library: manifest summaries with tag/text/state filters.
 * Channel byte sizes are stat'ed live so purge_native is reflected instantly.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>

static uint64_t dir_bytes(const char *dir, const char *sub)
{
    char path[720];
    /* a channel with zero records is absent, not "a few header bytes" —
     * keeps the GUI's bytes>0 checks (e.g. the drop-raw button) honest */
    struct stat ist;
    snprintf(path, sizeof path, "%s/%s/index.bin", dir, sub);
    if (stat(path, &ist) != 0 || ist.st_size < (off_t)sizeof(AirecIdxRow)) return 0;

    snprintf(path, sizeof path, "%s/%s", dir, sub);
    DIR *d = opendir(path);
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char f[980];
        snprintf(f, sizeof f, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(f, &st) == 0) total += (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

static int strcasestr_has(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* AND-of-tags: every csv tag must appear in the manifest tags array */
static int tags_match(const char *tags_json, const char *csv)
{
    if (!csv[0]) return 1;
    char tmp[TAGS_MAX_LEN];
    snprintf(tmp, sizeof tmp, "%s", csv);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char pat[80];
        snprintf(pat, sizeof pat, "\"%s\"", tok);
        if (!strstr(tags_json, pat)) return 0;
    }
    return 1;
}

void library_json(const char *qs, char *buf, size_t len)
{
    char f_tags[TAGS_MAX_LEN] = "", f_q[128] = "", f_state[32] = "";
    query_get(qs, "tags", f_tags, sizeof f_tags);
    query_get(qs, "q", f_q, sizeof f_q);
    query_get(qs, "state", f_state, sizeof f_state);

    char sids[256][SID_LEN + 1];
    int n = store_list_sids(g_rec.root, sids, 256);

    size_t o = 0;
    o += (size_t)snprintf(buf + o, len - o,
        "{\"disk_free_gb\":%.1f,\"disk_total_gb\":%.1f,\"sessions\":[",
        disk_free_gb(g_rec.root), disk_total_gb(g_rec.root));

    int emitted = 0;
    for (int i = 0; i < n && o + 4096 < len; i++) {
        char dir[640], mf[24576];
        snprintf(dir, sizeof dir, "%s/%.16s", g_rec.root, sids[i]);
        if (store_manifest_read(dir, mf, sizeof mf) < 0) continue;

        char state[32] = "", name[NAME_MAX_LEN * 2] = "", note[NOTE_MAX_LEN * 2] = "",
             tags[TAGS_MAX_LEN * 2] = "[]", dur[32] = "0", iso[48] = "",
             reason[32] = "", mode[16] = "";
        store_manifest_field(mf, "state", state, sizeof state);
        if (!strcmp(state, "recording") || !strcmp(state, "discarded")) continue;
        if (f_state[0] && strcmp(state, f_state)) continue;
        store_manifest_field(mf, "name", name, sizeof name);
        store_manifest_field(mf, "note", note, sizeof note);
        store_manifest_field(mf, "tags", tags, sizeof tags);
        store_manifest_field(mf, "dur_ms", dur, sizeof dur);
        store_manifest_field(mf, "iso8601", iso, sizeof iso);   /* first hit = t_start */
        store_manifest_field(mf, "stopped_reason", reason, sizeof reason);
        store_manifest_field(mf, "mode", mode, sizeof mode);

        if (!tags_match(tags, f_tags)) continue;
        if (f_q[0] && !strcasestr_has(name, f_q) && !strcasestr_has(note, f_q)) continue;

        /* store_manifest_field UN-escapes string fields, so name/note now hold raw
         * quotes/backslashes/newlines — re-escape before emitting or ONE session with
         * a quote in its name breaks the whole /library JSON. (tags is a verbatim
         * array; state/iso/mode/reason are controlled values — all JSON-safe.) */
        char ename[NAME_MAX_LEN * 3], enote[NOTE_MAX_LEN * 3];
        json_escape(name, ename, sizeof ename);
        json_escape(note, enote, sizeof enote);

        uint64_t b_native = dir_bytes(dir, "eo_y10");
        uint64_t b_disp = dir_bytes(dir, "eo_jpeg");
        uint64_t b_radar = dir_bytes(dir, "radar_raw") + dir_bytes(dir, "radar_wire");
        uint64_t b_meta = dir_bytes(dir, "events") + dir_bytes(dir, "det_wire");
        char tpath[700];
        snprintf(tpath, sizeof tpath, "%s/thumbs/0.jpg", dir);
        int has_thumbs = access(tpath, F_OK) == 0;

        o += (size_t)snprintf(buf + o, len - o,
            "%s{\"sid\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"t0\":\"%s\","
            "\"dur_ms\":%s,\"tags\":%s,\"note\":\"%s\",\"stopped_reason\":\"%s\","
            "\"mode\":\"%s\",\"thumbs\":%d,"
            "\"bytes\":{\"native\":%llu,\"display\":%llu,\"radar\":%llu,\"meta\":%llu}}",
            emitted ? "," : "", sids[i], ename, state, iso,
            dur[0] ? dur : "0", tags, enote, reason, mode, has_thumbs ? 8 : 0,
            (unsigned long long)b_native, (unsigned long long)b_disp,
            (unsigned long long)b_radar, (unsigned long long)b_meta);
        emitted++;
    }
    snprintf(buf + o, len - o, "]}\n");
}

int thumbs_serve_path(const char *sid, int n, char *path, size_t plen)
{
    if (strlen(sid) != SID_LEN || strchr(sid, '/')) return -1;
    snprintf(path, plen, "%s/%s/thumbs/%d.jpg", g_rec.root, sid, n);
    if (access(path, F_OK) == 0) return 0;
    /* lazy regeneration */
    char dir[640];
    snprintf(dir, sizeof dir, "%s/%s", g_rec.root, sid);
    if (thumbs_generate(dir) != 0) return -1;
    return access(path, F_OK) == 0 ? 0 : -1;
}
