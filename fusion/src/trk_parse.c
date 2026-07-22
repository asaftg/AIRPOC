#define _GNU_SOURCE
#include "trk_parse.h"
#include "jscan.h"
#include <string.h>

static int state_code(const char *o, const char *oe)
{
    const char *v = js_val(o, oe, "state");
    if (!v || *v != '"') return 0;
    v++;
    if (!strncmp(v, "conf", 4))  return 1;
    if (!strncmp(v, "coast", 5)) return 2;
    return 0;   /* tent */
}

static int cls_code(const char *o, const char *oe)
{
    const char *v = js_val(o, oe, "cls");
    if (!v || *v != '"') return 0;
    v++;
    if (!strncmp(v, "human", 5))   return 1;
    if (!strncmp(v, "vehicle", 7)) return 2;
    if (!strncmp(v, "drone", 5))   return 3;
    return 0;
}

int trk_parse(const char *json, FusEoTrk *out, int max, TrkMeta *meta)
{
    if (meta) {
        const char *hend = strstr(json, "\"tracks\"");
        if (!hend) hend = json + strlen(json);
        meta->frame_id = js_u64(json, hend, "frame_id");
        meta->t_pub_ns = js_u64(json, hend, "t_pub_ns");
        meta->t_out_ns = js_u64(json, hend, "t_out_ns");
        meta->connected = js_bool(json, hend, "connected", 0);
        const char *v = js_val(json, hend, "engaged");
        meta->engaged = v ? atoi(v) : -1;
        meta->img_w = (int)js_num(json, hend, "w", 0);
        meta->img_h = (int)js_num(json, hend, "h", 0);
        meta->ifov_rad = js_num(json, hend, "ifov_urad", 0) * 1e-6;
    }
    const char *a, *end;
    if (!js_array_bounds(json, "\"tracks\"", &a, &end)) return 0;
    int n = 0;
    const char *cur = a + 1, *o, *oe;
    while (n < max && js_next_obj(&cur, end, &o, &oe)) {
        FusEoTrk *t = &out[n];
        memset(t, 0, sizeof *t);
        const char *v = js_val(o, oe, "tid");
        if (!v) continue;
        t->tid = atoi(v);
        t->state = state_code(o, oe);
        t->cls = cls_code(o, oe);
        t->cls_conf = js_num(o, oe, "cls_conf", 0);
        t->conf = js_num(o, oe, "conf", 0);
        double ang[4], rate[2], sang[2];
        if (js_arr(o, oe, "ang", ang, 4)) { t->az = ang[0]; t->el = ang[1]; t->aw = ang[2]; t->ah = ang[3]; }
        if (js_arr(o, oe, "rate", rate, 2)) { t->vaz = rate[0]; t->vel = rate[1]; }
        if (js_arr(o, oe, "s_ang", sang, 2)) { t->s_az = sang[0]; t->s_el = sang[1]; }
        t->grow = js_num(o, oe, "grow", 0);
        t->coast_s = js_num(o, oe, "coast_s", 0);
        t->age_s = js_num(o, oe, "age_s", 0);
        t->hits = (int)js_num(o, oe, "hits", 0);
        const char *lk = js_val(o, oe, "lock");
        if (lk) { t->lock_on = js_bool(o, oe, "on", 0); t->lock_score = js_num(o, oe, "score", 0); }
        n++;
    }
    return n;
}
