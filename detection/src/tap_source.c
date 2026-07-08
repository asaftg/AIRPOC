/* tap_source.c — FrameSource backed by the EO shared-memory tap (airpoc.eo_y10).
 *
 * Drains the seqlock ring the recorder also reads (see recorder/src/channel.c
 * drain_heavy() for the reference pattern). The publisher never blocks on us;
 * if it laps us we skip and count the gap. We re-open the tap when the EO
 * pipeline restarts (the shm is unlinked + recreated, so a stale mapping just
 * stops advancing — we detect that via a caller-side staleness watchdog and
 * call close()+reopen).
 */
#include "source.h"
#include "airpoc_tap.h"
#include <stdlib.h>

/* Reopen the tap if it has been connected but silent this long — this is how we
 * recover from an EO-pipeline restart, which unlinks + recreates the shm and
 * leaves our old mapping stale (wseq stops advancing). */
#define TAP_STALE_NS  2000000000ull   /* 2 s */

typedef struct {
    char      name[64];
    AirTapSub sub;
    int       sub_ok;
    uint64_t  last_rx_ns;   /* last successful read (or reopen) time */
    /* One frame's worth of payload, filled by tap_read. Reused every call —
     * DetFrame.y10 points here and is valid until the next next(). */
    uint8_t   buf[EO_FRAME_BYTES];
} TapSrc;

static int tap_src_reopen(TapSrc *ts)
{
    if (ts->sub_ok) { tap_close(&ts->sub); ts->sub_ok = 0; }
    ts->last_rx_ns = tap_now_ns();          /* reset the staleness clock */
    if (tap_open(&ts->sub, ts->name) == 0) { ts->sub_ok = 1; return 1; }
    return 0;
}

static int tap_src_next(FrameSource *s, DetFrame *out)
{
    TapSrc *ts = s->impl;
    if (!ts->sub_ok) {
        if (!tap_src_reopen(ts)) return 0;   /* publisher not up yet; caller idles */
    }
    AirTapRec rec;
    int r = tap_read(&ts->sub, ts->buf, (uint32_t)sizeof ts->buf, &rec);
    if (r != 1) {
        if (tap_now_ns() - ts->last_rx_ns > TAP_STALE_NS) tap_src_reopen(ts);
        return 0;                            /* nothing new this poll */
    }
    ts->last_rx_ns = tap_now_ns();

    out->y10 = ts->buf;
    out->bytes = rec.payload_len;
    out->w = EO_IMG_W;
    out->h = EO_IMG_H;
    out->seq = rec.seq;
    out->t_src_ns = rec.t_src_ns;
    out->t_pub_ns = rec.t_pub_ns;
    memcpy(out->meta, rec.meta, sizeof out->meta);
    out->gap_before = rec.gap_before;
    return 1;
}

static void tap_src_close(FrameSource *s)
{
    TapSrc *ts = s->impl;
    if (ts) {
        if (ts->sub_ok) tap_close(&ts->sub);
        free(ts);
    }
    free(s);
}

static int tap_src_connected(const FrameSource *s)
{
    const TapSrc *ts = s->impl;
    return ts && ts->sub_ok;
}

FrameSource *tap_source_open(const char *tap_name)
{
    FrameSource *s = calloc(1, sizeof *s);
    TapSrc *ts = calloc(1, sizeof *ts);
    if (!s || !ts) { free(s); free(ts); return NULL; }
    snprintf(ts->name, sizeof ts->name, "%s", tap_name ? tap_name : EO_TAP_NAME);
    /* Try once now; a miss is fine — next() retries until the publisher appears. */
    tap_src_reopen(ts);
    s->impl = ts;
    s->next = tap_src_next;
    s->close = tap_src_close;
    s->connected = tap_src_connected;
    return s;
}
