#include "emit.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

/* Advance the write offset by an snprintf() return, clamped to the buffer.
 * snprintf returns the length it WOULD have written, which can exceed the space
 * left; clamping to cap keeps every later buf[off] access in bounds (the caller
 * still guards single-char writes with off < cap). */
static size_t adv(size_t off, size_t cap, int n)
{
    if (n < 0) return off;
    size_t no = off + (size_t)n;
    return no > cap ? cap : no;
}

/* Append one box object. az/el from the box centre; angular width/height are the
 * pixel extents scaled by IFOV. az is +right, el is +up. */
static size_t append_box(char *buf, size_t cap, size_t off, const DetBox *b, double ifov)
{
    if (off >= cap) return off;
    double az = (b->cx - EO_CX) * ifov;
    double el = (EO_CY - b->cy) * ifov;
    double aw = b->w * ifov;
    double ah = b->h * ifov;

    off = adv(off, cap, snprintf(buf + off, cap - off,
        "{\"src\":\"%s\",", b->src ? b->src : "app"));
    if (off >= cap) return off;
    if (b->cls)
        off = adv(off, cap, snprintf(buf + off, cap - off, "\"cls\":\"%s\",", b->cls));
    if (off >= cap) return off;
    if (b->age >= 0)
        off = adv(off, cap, snprintf(buf + off, cap - off, "\"age\":%d,", b->age));
    if (off >= cap) return off;
    if (b->hits >= 0)
        off = adv(off, cap, snprintf(buf + off, cap - off, "\"hits\":%d,", b->hits));
    if (off >= cap) return off;
    if (b->disp >= 0)
        off = adv(off, cap, snprintf(buf + off, cap - off, "\"disp\":%.1f,", b->disp));
    if (off >= cap) return off;
    if (b->tbd)
        off = adv(off, cap, snprintf(buf + off, cap - off, "\"tbd\":1,"));
    if (off >= cap) return off;
    if (b->dtid)
        off = adv(off, cap, snprintf(buf + off, cap - off, "\"dtid\":%u,", b->dtid));
    if (off >= cap) return off;
    off = adv(off, cap, snprintf(buf + off, cap - off,
        "\"conf\":%.3f,\"px\":[%.1f,%.1f,%.1f,%.1f],"
        "\"ang\":[%.4f,%.4f,%.4f,%.4f]}",
        b->conf, b->cx, b->cy, b->w, b->h, az, el, aw, ah));
    return off;
}

static size_t append_array(char *buf, size_t cap, size_t off, const char *key,
                           const DetBox *arr, int n, double ifov)
{
    if (off >= cap) return off;
    off = adv(off, cap, snprintf(buf + off, cap - off, "\"%s\":[", key));
    for (int i = 0; i < n && off < cap; i++) {
        if (i && off < cap) buf[off++] = ',';
        off = append_box(buf, cap, off, &arr[i], ifov);
    }
    if (off < cap) buf[off++] = ']';
    return off;
}

size_t det_frame_json(char *buf, size_t cap, const DetHdr *h,
                      const DetBox *dets, int n_dets,
                      const DetBox *movers, int n_movers)
{
    if (cap == 0) return 0;
    size_t off = 0;
    off = adv(off, cap, snprintf(buf, cap,
        "{\"type\":\"det\",\"frame_id\":%llu,"
        "\"t_src_ns\":%llu,\"t_pub_ns\":%llu,\"t_out_ns\":%llu,"
        "\"latency_ms\":%.2f,"
        "\"night\":%s,\"illum\":{\"on\":%s,\"present\":%s,\"power\":%u,\"fov10\":%u},"
        "\"model\":\"%s\",\"img\":{\"w\":%d,\"h\":%d},\"ifov_urad\":%.1f,"
        "\"tap_gaps\":%lu,\"drops_cum\":%lu,",
        (unsigned long long)h->frame_id,
        (unsigned long long)h->t_src_ns, (unsigned long long)h->t_pub_ns,
        (unsigned long long)h->t_out_ns,
        /* latency = detector pipeline time (frame published by EO -> we emit).
         * Measured from t_pub_ns, NOT t_src_ns: the IMX296/V4L2 driver stamps
         * t_src on a clock that is NOT CLOCK_MONOTONIC-comparable (observed ~30 s
         * ahead of tap_now_ns), so t_src is only a frame-correlation key here.
         * t_pub and t_out are both CLOCK_MONOTONIC (systemwide) -> a valid diff.
         * NOTE this is per-tick pipeline latency; a box promoted by temporal
         * integration ("tbd":1) additionally waited `age` ticks for its evidence. */
        h->t_out_ns >= h->t_pub_ns ? (h->t_out_ns - h->t_pub_ns) / 1e6 : 0.0,
        h->night ? "true" : "false",
        h->illum_on ? "true" : "false", h->illum_present ? "true" : "false",
        h->illum_power, h->illum_fov10,
        h->model ? h->model : "none", h->img_w, h->img_h,
        h->ifov_rad * 1e6, h->tap_gaps, h->drops_cum));
    if (off >= cap) { buf[cap - 1] = 0; return off; }

    off = append_array(buf, cap, off, "dets", dets, n_dets, h->ifov_rad);
    if (off < cap) buf[off++] = ',';
    off = append_array(buf, cap, off, "movers", movers, n_movers, h->ifov_rad);
    if (off < cap) buf[off++] = '}';
    if (off < cap) buf[off] = 0; else buf[cap - 1] = 0;
    return off;
}
