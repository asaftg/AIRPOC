#include "eo_reader.h"
#include "config.h"
#include "airpoc_tap.h"
#include <stdlib.h>
#include <string.h>

#define EO_STALE_NS 2000000000ull   /* reopen if connected but silent 2 s */

struct EoReader {
    char      name[64];
    AirTapSub sub;
    int       ok;
    uint64_t  last_rx_ns;
    uint8_t   scratch[EO_FRAME_BYTES];
};

static int reopen(EoReader *r)
{
    if (r->ok) { tap_close(&r->sub); r->ok = 0; }
    r->last_rx_ns = tap_now_ns();
    if (tap_open(&r->sub, r->name) == 0) { r->ok = 1; return 1; }
    return 0;
}

EoReader *eo_open(const char *tap_name)
{
    EoReader *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    snprintf(r->name, sizeof r->name, "%s", tap_name ? tap_name : EO_TAP_NAME);
    reopen(r);
    return r;
}

void eo_close(EoReader *r)
{
    if (!r) return;
    if (r->ok) tap_close(&r->sub);
    free(r);
}

int eo_connected(const EoReader *r) { return r && r->ok; }

int eo_latest(EoReader *r, uint16_t *buf, size_t cap,
              int *w, int *h, uint64_t *seq, uint64_t *t_src_ns, uint32_t meta[6])
{
    if (!r) return 0;
    if (!r->ok && !reopen(r)) return 0;
    if (r->ok && tap_stale(&r->sub)) reopen(r);

    AirTapRec rec; int got = 0;
    /* drain to the newest record */
    for (;;) {
        int rc = tap_read(&r->sub, r->scratch, (uint32_t)sizeof r->scratch, &rec);
        if (rc == 1) { got = 1; continue; }
        if (rc == 0) break;
        if (rc < 0) { reopen(r); return 0; }
    }
    if (!got) {
        if (tap_now_ns() - r->last_rx_ns > EO_STALE_NS) reopen(r);
        return 0;
    }
    r->last_rx_ns = tap_now_ns();
    size_t n = rec.payload_len < cap ? rec.payload_len : cap;
    memcpy(buf, r->scratch, n);
    if (w) *w = EO_IMG_W;
    if (h) *h = EO_IMG_H;
    if (seq) *seq = rec.seq;
    if (t_src_ns) *t_src_ns = rec.t_src_ns;
    if (meta) memcpy(meta, rec.meta, sizeof(uint32_t) * 6);
    return 1;
}
