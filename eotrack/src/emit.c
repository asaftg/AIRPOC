#include "emit.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

static const char *CLS_NAME[4] = { "unknown", "human", "vehicle", "drone" };

static size_t adv(size_t off, size_t cap, int n)
{
    if (n < 0) return off;
    size_t no = off + (size_t)n;
    return no > cap ? cap : no;
}

/* One track object. Position/size and rates are converted from px to radians here;
 * everything else is passed through. Sigma becomes an angular sigma (saz==sel: the
 * EO position uncertainty is isotropic in px). */
static size_t append_track(char *buf, size_t cap, size_t off, const TrkOut *t, double ifov)
{
    if (off >= cap) return off;
    double az = (t->cx - EO_CX) * ifov;
    double el = (EO_CY - t->cy) * ifov;
    double aw = t->w * ifov, ah = t->h * ifov;
    double vaz = t->vx * ifov;          /* rad/s */
    double vel = -t->vy * ifov;         /* +el is up = -y */
    double sig = t->s_px * ifov;
    const char *cls = (t->cls >= 0 && t->cls < 4) ? CLS_NAME[t->cls] : "unknown";
    const char *src = t->src == 2 ? "both" : (t->src == 1 ? "mot" : "app");

    off = adv(off, cap, snprintf(buf + off, cap - off,
        "{\"tid\":%d,\"state\":\"%s\",\"cls\":\"%s\",\"cls_conf\":%.2f,\"conf\":%.3f,"
        "\"px\":[%.1f,%.1f,%.1f,%.1f],\"ang\":[%.4f,%.4f,%.4f,%.4f],"
        "\"rate\":[%.4f,%.4f],\"s_ang\":[%.4f,%.4f],\"grow\":%.3f,"
        "\"hits\":%d,\"age_s\":%.2f,\"coast_s\":%.2f,\"t_meas_ns\":%llu,\"src\":\"%s\"",
        t->tid, t->state, cls, t->cls_conf, t->conf,
        t->cx, t->cy, t->w, t->h, az, el, aw, ah,
        vaz, vel, sig, sig, t->grow,
        t->hits, t->age_s, t->coast_s, (unsigned long long)t->t_meas_ns, src));
    if (off >= cap) return off;
    if (t->tbd)
        off = adv(off, cap, snprintf(buf + off, cap - off, ",\"tbd\":1"));
    if (off >= cap) return off;
    if (t->lock_on)
        off = adv(off, cap, snprintf(buf + off, cap - off,
            ",\"lock\":{\"on\":true,\"score\":%.3f}", t->lock_score));
    if (off < cap) buf[off++] = '}';
    return off;
}

size_t trk_frame_json(char *buf, size_t cap, const TrkHdr *h,
                      const TrkOut *tracks, int n)
{
    if (cap == 0) return 0;
    size_t off = 0;
    off = adv(off, cap, snprintf(buf, cap,
        "{\"type\":\"trk\",\"connected\":%s,\"mode\":\"%s\",\"engaged\":%d,"
        "\"frame_id\":%llu,\"t_src_ns\":%llu,\"t_pub_ns\":%llu,\"t_out_ns\":%llu,"
        "\"img\":{\"w\":%d,\"h\":%d},\"ifov_urad\":%.1f,\"tracks\":[",
        h->connected ? "true" : "false", h->mode ? h->mode : "stare", h->engaged,
        (unsigned long long)h->frame_id,
        (unsigned long long)h->t_src_ns, (unsigned long long)h->t_pub_ns,
        (unsigned long long)h->t_out_ns,
        h->img_w, h->img_h, h->ifov_rad * 1e6));
    for (int i = 0; i < n && off < cap; i++) {
        if (i && off < cap) buf[off++] = ',';
        off = append_track(buf, cap, off, &tracks[i], h->ifov_rad);
    }
    if (off < cap) buf[off++] = ']';
    if (off < cap) buf[off++] = '}';
    if (off < cap) buf[off] = 0; else buf[cap - 1] = 0;
    return off;
}
