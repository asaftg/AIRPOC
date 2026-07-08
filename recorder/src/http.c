/* http.c — :8093 surface. Same shape as the rest of the repo: hand-rolled
 * HTTP/1.0, pthread per connection, GET only, Connection: close.
 * The app's gui.c reaches everything here through its /rec/ pass-through.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

void urldecode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else if (*s == '+') { *o++ = ' '; s++; }
        else *o++ = *s++;
    }
    *o = 0;
}

int query_get(const char *qs, const char *key, char *out, size_t outlen)
{
    out[0] = 0;
    size_t kl = strlen(key);
    const char *p = qs;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < outlen - 1) out[i++] = *p++;
            out[i] = 0;
            urldecode(out);
            return 0;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return -1;
}

static void send_head(int fd, int code, const char *status, const char *ctype, long clen)
{
    char h[256];
    int n = snprintf(h, sizeof h,
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\n%s%ld%s"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, status, ctype,
        clen >= 0 ? "Content-Length: " : "", clen >= 0 ? clen : 0,
        clen >= 0 ? "\r\n" : "\r\n");
    if (write(fd, h, (size_t)n) != n) {}
}

static void send_json(int fd, int code, const char *body)
{
    send_head(fd, code, code == 200 ? "OK" : "Bad Request", "application/json", (long)strlen(body));
    if (write(fd, body, strlen(body)) < 0) {}
}

static void send_file(int fd, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "rb");
    if (!f) { send_json(fd, 404, "{\"err\":\"not found\"}"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    send_head(fd, 200, "OK", ctype, sz);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (write(fd, buf, n) != (ssize_t)n) break;
    fclose(f);
}

/* Range-capable file server for <video> seeking. Honors a single
 * "Range: bytes=start-end"; otherwise serves the whole file (Accept-Ranges). */
static void send_file_range(int fd, const char *path, const char *ctype, const char *range)
{
    FILE *f = fopen(path, "rb");
    if (!f) { send_json(fd, 404, "{\"err\":\"not found\"}"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);

    long start = 0, end = sz - 1;
    int partial = 0;
    if (range && !strncmp(range, "bytes=", 6)) {
        char *dash = strchr(range + 6, '-');
        start = atol(range + 6);
        if (dash && dash[1]) end = atol(dash + 1);
        if (start < 0) start = 0;
        if (end >= sz) end = sz - 1;
        if (end < start) { start = 0; end = sz - 1; }
        partial = 1;
    }
    long clen = end - start + 1;

    char h[320];
    int hn;
    if (partial)
        hn = snprintf(h, sizeof h,
            "HTTP/1.0 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\nContent-Length: %ld\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n", ctype, start, end, sz, clen);
    else
        hn = snprintf(h, sizeof h,
            "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
            "Content-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", ctype, sz);
    if (write(fd, h, (size_t)hn) != hn) { fclose(f); return; }

    fseek(f, start, SEEK_SET);
    char buf[65536];
    long left = clen;
    while (left > 0) {
        size_t want = left < (long)sizeof buf ? (size_t)left : sizeof buf;
        size_t n = fread(buf, 1, want, f);
        if (!n || write(fd, buf, n) != (ssize_t)n) break;
        left -= (long)n;
    }
    fclose(f);
}

/* ---- /export: stream selected sessions as a .tar download ---- */

static int valid_sid(const char *s)
{
    if (strlen(s) != SID_LEN) return 0;
    for (int i = 0; i < SID_LEN; i++) {
        char c = s[i];
        int ok = (i == 8) ? c == 'T' : (i == SID_LEN - 1) ? c == 'Z' : (c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

static void handle_export(int fd, const char *qs)
{
    char sids[8192] = "", tier[16] = "";
    query_get(qs, "sids", sids, sizeof sids);
    query_get(qs, "tier", tier, sizeof tier);
    if (!sids[0]) { send_json(fd, 400, "{\"err\":\"sids= required (or sids=all)\"}"); return; }

    /* sids=all -> every saved/pending session on disk */
    if (!strcmp(sids, "all")) {
        static char all[256][SID_LEN + 1];
        int na = store_list_sids(g_rec.root, all, 256);
        size_t o = 0;
        for (int i = 0; i < na; i++) o += (size_t)snprintf(sids + o, sizeof sids - o, "%s%s", i ? "," : "", all[i]);
        if (!sids[0]) { send_json(fd, 404, "{\"err\":\"no sessions\"}"); return; }
    }

    if (strcmp(tier, "meta") && strcmp(tier, "full")) snprintf(tier, sizeof tier, "display");
    int want_video = strcmp(tier, "meta") != 0;      /* display/full carry the movie */

    /* validate every sid (charset-checked -> safe for the shell) + exists; and for
     * video tiers, make sure the playable native.mp4 is built (sync) before tarring */
    char list[8192]; size_t lo = 0; int nsid = 0;
    char work[8192]; snprintf(work, sizeof work, "%s", sids);
    char *save = NULL;
    for (char *tok = strtok_r(work, ",", &save); tok && nsid < 256; tok = strtok_r(NULL, ",", &save)) {
        if (!valid_sid(tok)) { send_json(fd, 400, "{\"err\":\"bad sid\"}"); return; }
        char probe[700];
        snprintf(probe, sizeof probe, "%s/%s/manifest.json", g_rec.root, tok);
        if (access(probe, F_OK) != 0) continue;
        if (want_video) transcode_ensure(tok);        /* build the EO native.mp4 if missing */
        lo += (size_t)snprintf(list + lo, sizeof list - lo, " '%s'", tok);
        nsid++;
    }
    if (!nsid) { send_json(fd, 404, "{\"err\":\"no such sessions\"}"); return; }

    /* meta = no video · display = the playable movie + metadata (no raw channels)
     * · full = everything (raw channels + movie) */
    const char *excl;
    if (!strcmp(tier, "meta"))      excl = "--exclude=*/eo_y10 --exclude=*/eo_jpeg --exclude=*.mp4 --exclude=*.tmp";
    else if (!strcmp(tier, "full")) excl = "--exclude=*.tmp";
    else                            excl = "--exclude=*/eo_y10 --exclude=*/eo_jpeg --exclude=*.tmp";

    char cmd[8192];
    int cn = snprintf(cmd, sizeof cmd, "tar -C '%s' -cf - %s%s", g_rec.root, excl, list);
    if (cn <= 0 || cn >= (int)sizeof cmd) { send_json(fd, 500, "{\"err\":\"cmd\"}"); return; }

    char h[256];
    int hn = snprintf(h, sizeof h,
        "HTTP/1.0 200 OK\r\nContent-Type: application/x-tar\r\n"
        "Content-Disposition: attachment; filename=\"airpoc-%s-%dsession%s.tar\"\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        tier, nsid, nsid == 1 ? "" : "s");
    if (write(fd, h, (size_t)hn) != hn) return;

    FILE *p = popen(cmd, "r");
    if (!p) return;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0)
        if (write(fd, buf, n) != (ssize_t)n) break;
    pclose(p);
}

/* ---- /ctl ---- */

static void handle_ctl(int fd, const char *qs)
{
    char v[NOTE_MAX_LEN * 2], resp[256];

    if (query_get(qs, "rec", v, sizeof v) == 0) {
        if (!strcmp(v, "start")) {
            char err[128];
            if (session_start(err, sizeof err) == 0)
                snprintf(resp, sizeof resp, "{\"ok\":1,\"sid\":\"%s\"}", g_rec.sid);
            else
                snprintf(resp, sizeof resp, "{\"ok\":0,\"err\":\"%s\"}", err);
            send_json(fd, 200, resp);
            return;
        }
        if (!strcmp(v, "stop")) {
            char sid[SID_LEN + 1];
            snprintf(sid, sizeof sid, "%s", g_rec.sid);
            if (session_stop("operator") == 0)
                snprintf(resp, sizeof resp, "{\"ok\":1,\"sid\":\"%s\"}", sid);
            else
                snprintf(resp, sizeof resp, "{\"ok\":0,\"err\":\"not recording\"}");
            send_json(fd, 200, resp);
            return;
        }
        send_json(fd, 400, "{\"err\":\"rec=start|stop\"}");
        return;
    }
    if (query_get(qs, "save", v, sizeof v) == 0) {
        char name[NAME_MAX_LEN] = "", tags[TAGS_MAX_LEN] = "", note[NOTE_MAX_LEN] = "";
        query_get(qs, "name", name, sizeof name);
        query_get(qs, "tags", tags, sizeof tags);
        query_get(qs, "note", note, sizeof note);
        send_json(fd, 200, session_save(v, name, tags, note) == 0
                           ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"save failed\"}");
        return;
    }
    if (query_get(qs, "discard", v, sizeof v) == 0) {
        send_json(fd, 200, session_discard(v) == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
        return;
    }
    if (query_get(qs, "delete", v, sizeof v) == 0) {
        snprintf(resp, sizeof resp, "{\"ok\":1,\"deleted\":%d}", session_delete(v));
        send_json(fd, 200, resp);
        return;
    }
    if (query_get(qs, "purge_native", v, sizeof v) == 0) {
        send_json(fd, 200, session_purge_native(v) == 0 ? "{\"ok\":1}" : "{\"ok\":0}");
        return;
    }
    if (query_get(qs, "marker", v, sizeof v) == 0) {
        session_marker(v);
        send_json(fd, 200, "{\"ok\":1}");
        return;
    }
    if (query_get(qs, "mode", v, sizeof v) == 0) {
        VideoMode m = !strcmp(v, "raw16") ? MODE_RAW16 : !strcmp(v, "y8") ? MODE_Y8 : MODE_Y10P;
        __atomic_store_n((int *)&g_rec.mode, (int)m, __ATOMIC_RELAXED);
        send_json(fd, 200, "{\"ok\":1}");
        return;
    }
    if (query_get(qs, "keep", v, sizeof v) == 0) {
        int k = atoi(v);
        __atomic_store_n(&g_rec.keep, k > 0 ? k : 1, __ATOMIC_RELAXED);
        send_json(fd, 200, "{\"ok\":1}");
        return;
    }
    send_json(fd, 400, "{\"err\":\"unknown ctl\"}");
}

/* ---- connection handler ---- */

static void handle(int fd, const char *path, const char *qs, const char *range)
{
    static char big[1024 * 1024];        /* >= eo_jpeg slot cap; guarded by g_big_lk */
    static pthread_mutex_t g_big_lk = PTHREAD_MUTEX_INITIALIZER;

    if (!strcmp(path, "/stats")) {
        char buf[8192];
        session_stats_json(buf, sizeof buf);
        send_json(fd, 200, buf);
    } else if (!strcmp(path, "/ctl")) {
        handle_ctl(fd, qs);
    } else if (!strcmp(path, "/library") || !strcmp(path, "/sessions")) {
        pthread_mutex_lock(&g_big_lk);
        library_json(qs, big, sizeof big);
        send_json(fd, 200, big);
        pthread_mutex_unlock(&g_big_lk);
    } else if (!strncmp(path, "/session/", 9)) {
        char dir[640], buf[24576];
        if (strlen(path + 9) == SID_LEN && !strchr(path + 9, '/') &&
            snprintf(dir, sizeof dir, "%s/%s", g_rec.root, path + 9) > 0 &&
            store_manifest_read(dir, buf, sizeof buf) > 0)
            send_json(fd, 200, buf);
        else
            send_json(fd, 404, "{\"err\":\"no such session\"}");
    } else if (!strncmp(path, "/thumbs/", 8)) {
        char sid[SID_LEN + 1];
        int n = -1;
        if (sscanf(path + 8, "%16[0-9TZ]/%d.jpg", sid, &n) == 2 && n >= 0 && n < 8) {
            char p[700];
            if (thumbs_serve_path(sid, n, p, sizeof p) == 0) { send_file(fd, p, "image/jpeg"); return; }
        }
        send_json(fd, 404, "{\"err\":\"no thumb\"}");
    } else if (!strcmp(path, "/replay/ctl")) {
        char resp[256];
        replay_ctl(qs, resp, sizeof resp);
        send_json(fd, 200, resp);
    } else if (!strcmp(path, "/replay/state")) {
        char buf[1024];
        replay_state_json(buf, sizeof buf);
        send_json(fd, 200, buf);
    } else if (!strcmp(path, "/replay/stats")) {
        pthread_mutex_lock(&g_big_lk);
        replay_stats_json(big, sizeof big);
        send_json(fd, 200, big);
        pthread_mutex_unlock(&g_big_lk);
    } else if (!strcmp(path, "/replay/radar")) {
        pthread_mutex_lock(&g_big_lk);
        if (replay_radar_json(big, sizeof big) == 0) send_json(fd, 200, big);
        else send_json(fd, 404, "{\"connected\":false,\"replay\":true}");
        pthread_mutex_unlock(&g_big_lk);
    } else if (!strcmp(path, "/replay/det")) {
        pthread_mutex_lock(&g_big_lk);
        if (replay_det_json(big, sizeof big) == 0) send_json(fd, 200, big);
        else send_json(fd, 404, "{\"connected\":false,\"replay\":true}");
        pthread_mutex_unlock(&g_big_lk);
    } else if (!strcmp(path, "/replay/rstats")) {
        pthread_mutex_lock(&g_big_lk);
        if (replay_rstats_json(big, sizeof big) == 0) send_json(fd, 200, big);
        else send_json(fd, 404, "{\"connected\":false,\"replay\":true}");
        pthread_mutex_unlock(&g_big_lk);
    } else if (!strcmp(path, "/replay/frame")) {
        char v[32];
        uint32_t jlen;
        pthread_mutex_lock(&g_big_lk);
        if (query_get(qs, "t", v, sizeof v) == 0 &&
            replay_frame_copy(atoll(v), (uint8_t *)big, sizeof big, &jlen) == 0) {
            send_head(fd, 200, "OK", "image/jpeg", (long)jlen);
            if (write(fd, big, jlen) != (ssize_t)jlen) {}
        } else
            send_json(fd, 404, "{\"err\":\"no frame\"}");
        pthread_mutex_unlock(&g_big_lk);
    } else if (!strcmp(path, "/replay/native.mp4")) {
        char sid[SID_LEN + 1] = "", p[640];
        query_get(qs, "sid", sid, sizeof sid);
        if (!sid[0]) { send_json(fd, 400, "{\"err\":\"sid=\"}"); return; }
        int pct = 0, st = transcode_status(sid, &pct);
        if (st == 2 && transcode_mp4_path(sid, p, sizeof p) == 0)
            send_file_range(fd, p, "video/mp4", range);
        else if (st == 1) {                               /* still encoding */
            char b[64]; snprintf(b, sizeof b, "{\"building\":true,\"pct\":%d}", pct);
            send_head(fd, 202, "Accepted", "application/json", (long)strlen(b));
            if (write(fd, b, strlen(b)) < 0) {}
        } else {
            transcode_request(sid);                       /* kick it, tell client to wait */
            send_json(fd, 202, "{\"building\":true,\"pct\":0}");
        }
    } else if (!strcmp(path, "/export")) {
        handle_export(fd, qs);                            /* selected sessions -> .tar download */
    } else if (!strcmp(path, "/replay/stream")) {
        replay_stream(fd);                                /* blocks for connection life */
    } else {
        send_json(fd, 404, "{\"err\":\"no route\"}");
    }
}

typedef struct { int fd; } Client;

static void *client_thread(void *arg)
{
    int fd = ((Client *)arg)->fd;
    free(arg);
    char req[2048];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n > 0) {
        req[n] = 0;
        char path[1024] = "";
        if (sscanf(req, "GET %1023s HTTP", path) == 1) {
            char *qs = strchr(path, '?');
            if (qs) *qs++ = 0;
            /* pull the Range header value (case-insensitive) for <video> seeks */
            char range[64] = "";
            for (char *h = req; (h = strchr(h, '\n')); h++) {
                if (!strncasecmp(h + 1, "Range:", 6)) {
                    char *v = h + 7;
                    while (*v == ' ') v++;
                    size_t i = 0;
                    while (v[i] && v[i] != '\r' && v[i] != '\n' && i < sizeof range - 1) { range[i] = v[i]; i++; }
                    range[i] = 0;
                    break;
                }
            }
            handle(fd, path, qs ? qs : "", range);
        }
    }
    close(fd);
    return NULL;
}

static void *accept_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        Client *c = malloc(sizeof *c);
        c->fd = fd;
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, c) == 0) pthread_detach(t);
        else { close(fd); free(c); }
    }
    return NULL;
}

int httpd_start(int port)
{
    signal(SIGPIPE, SIG_IGN);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(lfd, 16) != 0) {
        fprintf(stderr, "rec: cannot listen on :%d: %s\n", port, strerror(errno));
        close(lfd);
        return -1;
    }
    pthread_t t;
    pthread_create(&t, NULL, accept_thread, (void *)(intptr_t)lfd);
    return 0;
}
