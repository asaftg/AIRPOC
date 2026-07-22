#define _GNU_SOURCE
#include "trim.h"
#include "config.h"
#include "jscan.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int load_from(const char *path, double *az, double *el)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *end = buf + n;
    const char *va = js_val(buf, end, "az_deg"), *ve = js_val(buf, end, "el_deg");
    if (!va || !ve) return 0;
    *az = atof(va); *el = atof(ve);
    return 1;
}

int trim_load(double *az_deg, double *el_deg)
{
    if (load_from(FUS_TRIM_FILE, az_deg, el_deg)) return 1;
    return load_from(FUS_TRIM_FILE_FALLBACK, az_deg, el_deg);
}

static int save_to(const char *path, double az, double el)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "{\"az_deg\":%.2f,\"el_deg\":%.2f}\n", az, el);
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int trim_save(double az_deg, double el_deg)
{
    if (save_to(FUS_TRIM_FILE, az_deg, el_deg) == 0) return 0;
    return save_to(FUS_TRIM_FILE_FALLBACK, az_deg, el_deg);
}
