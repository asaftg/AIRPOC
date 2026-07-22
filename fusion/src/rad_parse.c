#define _GNU_SOURCE
#include "rad_parse.h"
#include "jscan.h"
#include <string.h>

int rad_parse(const char *json, FusRadTgt *out, int max, RadMeta *meta)
{
    if (meta) {
        /* header keys all sit before the points[] array - slice up to it so a
         * "timestamp" inside a future payload can't confuse the scan */
        const char *hend = strstr(json, "\"points\"");
        if (!hend) hend = strstr(json, "\"targets\"");
        if (!hend) hend = json + strlen(json);
        meta->frame_id = js_u64(json, hend, "frame_id");
        meta->connected = js_bool(json, hend, "connected", 0);
        meta->timestamp_s = js_num(json, hend, "timestamp", 0);
    }
    const char *a, *end;
    if (!js_array_bounds(json, "\"targets\"", &a, &end)) return 0;
    int n = 0;
    const char *cur = a + 1, *o, *oe;
    while (n < max && js_next_obj(&cur, end, &o, &oe)) {
        FusRadTgt *t = &out[n];
        memset(t, 0, sizeof *t);
        const char *v = js_val(o, oe, "tid");
        if (!v) continue;
        t->tid = atoi(v);
        t->x = js_num(o, oe, "x", 0); t->y = js_num(o, oe, "y", 0); t->z = js_num(o, oe, "z", 0);
        t->vx = js_num(o, oe, "vx", 0); t->vy = js_num(o, oe, "vy", 0); t->vz = js_num(o, oe, "vz", 0);
        t->sx = js_num(o, oe, "sx", 0.5); t->sy = js_num(o, oe, "sy", 0.5); t->sz = js_num(o, oe, "sz", 0.5);
        t->conf = js_num(o, oe, "conf", 0);
        t->np = (int)js_num(o, oe, "np", 0);
        t->sus = (int)js_num(o, oe, "sus", 0);
        t->mv = (int)js_num(o, oe, "mv_class", 0);
        n++;
    }
    return n;
}
