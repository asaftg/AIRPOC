/* store.c — session store: manifest read/write, sid enumeration, async delete.
 *
 * Manifests are written only by this daemon in a fixed layout, so reading uses
 * a minimal key scanner (same substring style as the rest of the repo) instead
 * of a JSON library.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

void store_write_file_atomic(const char *path, const char *data, size_t len)
{
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, data, len) != (ssize_t)len) { close(fd); unlink(tmp); return; }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) == 0) {
        char dir[640];
        snprintf(dir, sizeof dir, "%s", path);
        char *sl = strrchr(dir, '/');
        if (sl) {
            *sl = 0;
            int dfd = open(dir, O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) { fsync(dfd); close(dfd); }
        }
    }
}

int store_manifest_read(const char *dir, char *buf, size_t len)
{
    char path[640];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* Extract "key": <string|number|array> value as raw text (quotes stripped for
 * strings, brackets kept for arrays). Returns 0 on hit. */
int store_manifest_field(const char *json, const char *key, char *out, size_t outlen)
{
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    out[0] = 0;
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    size_t i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < outlen - 1) {
            if (*p == '\\' && p[1]) p++;
            out[i++] = *p++;
        }
    } else if (*p == '[') {
        int depth = 0;
        do {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            if (i < outlen - 1) out[i++] = *p;
            p++;
        } while (*p && depth > 0);
        if (depth == 0 && i < outlen - 1 && *(p - 1) != ']') out[i++] = ']';
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\n' && i < outlen - 1) out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

int store_manifest_set_state(const char *dir, const char *state)
{
    char buf[16384], cur[32];
    if (store_manifest_read(dir, buf, sizeof buf) < 0) return -1;
    if (store_manifest_field(buf, "state", cur, sizeof cur) != 0) return -1;

    char *p = strstr(buf, "\"state\":\"");
    if (!p) return -1;
    p += 9;
    char out[16384];
    size_t head = (size_t)(p - buf);
    int n = snprintf(out, sizeof out, "%.*s%s%s", (int)head, buf, state, p + strlen(cur));
    if (n < 0 || (size_t)n >= sizeof out) return -1;

    char path[640];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    store_write_file_atomic(path, out, (size_t)n);
    return 0;
}

static int sid_valid(const char *s)
{
    if (strlen(s) != SID_LEN) return 0;
    for (int i = 0; i < SID_LEN; i++) {
        char c = s[i];
        int ok = (i == 8) ? c == 'T' : (i == SID_LEN - 1) ? c == 'Z' : isdigit((unsigned char)c);
        if (!ok) return 0;
    }
    return 1;
}

int store_list_sids(const char *root, char sids[][SID_LEN + 1], int max)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)))
        if (e->d_name[0] != '.' && sid_valid(e->d_name))
            snprintf(sids[n++], SID_LEN + 1, "%s", e->d_name);
    closedir(d);
    /* newest first */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(sids[i], sids[j]) < 0) {
                char t[SID_LEN + 1];
                memcpy(t, sids[i], sizeof t);
                memcpy(sids[i], sids[j], sizeof t);
                memcpy(sids[j], t, sizeof t);
            }
    return n;
}

static void *rm_thread(void *arg)
{
    char *dir = arg;
    char cmd[720];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "rec: delete failed: %s\n", dir);
    free(dir);
    return NULL;
}

/* rename to .deleting-<basename> first so a crash mid-delete is unambiguous;
 * leftovers are swept on daemon start */
void store_rm_rf_async(const char *dir)
{
    char hidden[640];
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    snprintf(hidden, sizeof hidden, "%.*s.deleting-%s", (int)(base - dir), dir, base);
    const char *target = rename(dir, hidden) == 0 ? hidden : dir;

    pthread_t t;
    char *copy = strdup(target);
    if (pthread_create(&t, NULL, rm_thread, copy) == 0) pthread_detach(t);
    else { free(copy); }
}
