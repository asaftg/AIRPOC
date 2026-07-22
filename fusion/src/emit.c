/* emit.c - fused wire JSON. Angles are radians in the rig frame (az +right,
 * el +up; radar-sourced angles already carry the mount trim - consumers must
 * NOT add their own). Absent-side numerics are -1; "lock" appears only on the
 * engaged EO constituent. rdot is dr/dt: NEGATIVE = closing.
 */
#define _GNU_SOURCE
#include "emit.h"
#include <stdio.h>

static const char *CLS_NAME[4] = { "unknown", "human", "vehicle", "drone" };
static const char *SRC_NAME[3] = { "rad", "eo", "fus" };

static size_t adv(size_t off, size_t cap, int n)
{
    if (n < 0) return off;
    size_t no = off + (size_t)n;
    return no > cap ? cap : no;
}

static size_t append_row(char *buf, size_t cap, size_t off, const FusOut *o)
{
    if (off >= cap) return off;
    const char *cls = (o->cls >= 0 && o->cls < 4) ? CLS_NAME[o->cls] : "unknown";
    const char *src = SRC_NAME[o->src];

    off = adv(off, cap, snprintf(buf + off, cap - off,
        "{\"gid\":%u,\"src\":\"%s\",\"eo_tid\":%d,\"rad_tid\":%d,"
        "\"ang\":[%.4f,%.4f,%.4f,%.4f],\"ang_src\":\"%s\","
        "\"rate\":[%.4f,%.4f],\"r_m\":%.1f,\"rdot_mps\":%.2f,\"r_stale\":%d,"
        "\"cls\":\"%s\",\"cls_conf\":%.2f,\"conf\":%.3f,"
        "\"fused_age_s\":%.2f,\"eo_coast_s\":%.2f,\"rad_coast_s\":%.2f,"
        "\"grow\":%.3f,\"eo_hits\":%d,\"rad_np\":%d,\"sus\":%d,\"mv\":%d",
        o->gid, src, o->eo_tid, o->rad_tid,
        o->az, o->el, o->aw, o->ah, o->ang_src ? "eo" : "rad",
        o->vaz, o->vel, o->r_m, o->rdot_mps, o->r_stale,
        cls, o->cls_conf, o->conf,
        o->fused_age_s, o->eo_coast_s, o->rad_coast_s,
        o->grow, o->eo_hits, o->rad_np, o->sus, o->mv));
    if (off >= cap) return off;
    if (o->lock_on)
        off = adv(off, cap, snprintf(buf + off, cap - off,
            ",\"lock\":{\"on\":true,\"score\":%.3f}", o->lock_score));
    if (off < cap) buf[off++] = '}';
    return off;
}

size_t fus_frame_json(char *buf, size_t cap, const FusHdr *h,
                      const FusOut *rows, int n)
{
    if (cap == 0) return 0;
    size_t off = 0;
    off = adv(off, cap, snprintf(buf, cap,
        "{\"type\":\"fus\",\"rad_connected\":%s,\"trk_connected\":%s,"
        "\"frame_id\":%llu,\"rad_frame_id\":%llu,\"eo_frame_id\":%llu,"
        "\"eo_engaged\":%d,\"t_out_ns\":%llu,\"targets\":[",
        h->rad_connected ? "true" : "false", h->trk_connected ? "true" : "false",
        (unsigned long long)h->frame_id,
        (unsigned long long)h->rad_frame_id, (unsigned long long)h->eo_frame_id,
        h->eo_engaged, (unsigned long long)h->t_out_ns));
    for (int i = 0; i < n && off < cap; i++) {
        if (i && off < cap) buf[off++] = ',';
        off = append_row(buf, cap, off, &rows[i]);
    }
    if (off < cap) buf[off++] = ']';
    if (off < cap) buf[off++] = '}';
    if (off < cap) buf[off] = 0; else buf[cap - 1] = 0;
    return off;
}
