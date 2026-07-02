#include "tlv.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const uint8_t TLV_MAGIC[8] = {0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07};

#define HEADER_SIZE    32          /* 8 x uint32 after the magic word */
#define ENVELOPE_SIZE  (8 + HEADER_SIZE)
#define TLV_HDR_SIZE   8           /* type(u32) + length(u32) */
#define POINT_SIZE     16          /* x,y,z,doppler float32 */
#define SIDEINFO_SIZE  4           /* snr,noise int16 */
#define MAX_PACKET_LEN (256 * 1024)
#define MAX_BUFFER     (1 * 1024 * 1024)

#define TLV_DETECTED_POINTS 1
#define TLV_SIDE_INFO       7
/* 308 TARGET_LIST / 309 TARGET_INDEX: emitted only by a gtrack-linked
 * firmware. Not produced by today's build; recognised, skipped. */

struct TLVStream {
    uint8_t *buf;
    size_t   len;      /* bytes currently held */
    size_t   cap;
};

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float rd_f32(const uint8_t *p) {
    uint32_t u = rd_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static int16_t rd_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

TLVStream *tlv_stream_new(void) {
    TLVStream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cap = 64 * 1024;
    s->buf = malloc(s->cap);
    if (!s->buf) { free(s); return NULL; }
    return s;
}

void tlv_stream_free(TLVStream *s) {
    if (!s) return;
    free(s->buf);
    free(s);
}

/* Consume `n` bytes off the front of the buffer. */
static void consume(TLVStream *s, size_t n) {
    if (n >= s->len) { s->len = 0; return; }
    memmove(s->buf, s->buf + n, s->len - n);
    s->len -= n;
}

/* Find TLV_MAGIC in [start, len); return offset or -1. */
static long find_magic(const uint8_t *buf, size_t len) {
    if (len < 8) return -1;
    for (size_t i = 0; i + 8 <= len; i++) {
        if (buf[i] == TLV_MAGIC[0] && memcmp(buf + i, TLV_MAGIC, 8) == 0)
            return (long)i;
    }
    return -1;
}

/* Parse one complete packet at buf[0..plen). Fills points; returns
 * n_points (>=0) on success, -1 on a sanity failure. */
static int parse_packet(const uint8_t *buf, size_t plen, uint32_t *frame_number,
                        RadarPoint *out, int max_out) {
    if (plen < ENVELOPE_SIZE) return -1;
    if (memcmp(buf, TLV_MAGIC, 8) != 0) return -1;

    const uint8_t *h = buf + 8;
    uint32_t total_len = rd_u32(h + 4);
    uint32_t frame_no  = rd_u32(h + 12);
    uint32_t num_tlvs  = rd_u32(h + 24);
    if (total_len < ENVELOPE_SIZE || total_len > plen) return -1;

    int n_pts = 0;
    /* SideInfo is applied positionally on top of DetectedPoints. We fill
     * points first; if a SideInfo TLV follows, we overwrite snr/noise. */
    size_t cursor = ENVELOPE_SIZE;
    for (uint32_t t = 0; t < num_tlvs; t++) {
        if (cursor + TLV_HDR_SIZE > total_len) break;
        uint32_t type = rd_u32(buf + cursor);
        uint32_t tlen = rd_u32(buf + cursor + 4);
        cursor += TLV_HDR_SIZE;
        if (cursor + tlen > total_len) break;
        const uint8_t *pl = buf + cursor;
        cursor += tlen;

        if (type == TLV_DETECTED_POINTS) {
            int n = (int)(tlen / POINT_SIZE);
            for (int i = 0; i < n && n_pts < max_out; i++) {
                const uint8_t *rec = pl + (size_t)i * POINT_SIZE;
                float x = rd_f32(rec), y = rd_f32(rec + 4);
                float z = rd_f32(rec + 8), d = rd_f32(rec + 12);
                RadarPoint *p = &out[n_pts++];
                p->x = x; p->y = y; p->z = z; p->doppler = d;
                p->snr = NAN; p->noise = NAN;
                p->range = sqrtf(x * x + y * y + z * z);
                p->az = (x != 0.0f || y != 0.0f)
                        ? atan2f(x, y) * 180.0f / (float)M_PI : 0.0f;
                float horiz = sqrtf(x * x + y * y);
                p->el = (horiz > 1e-6f)
                        ? atan2f(z, horiz) * 180.0f / (float)M_PI : 0.0f;
                p->tid = 255;
            }
        } else if (type == TLV_SIDE_INFO) {
            int n = (int)(tlen / SIDEINFO_SIZE);
            for (int i = 0; i < n && i < n_pts; i++) {
                const uint8_t *rec = pl + (size_t)i * SIDEINFO_SIZE;
                out[i].snr   = rd_i16(rec)     * 0.1f;
                out[i].noise = rd_i16(rec + 2) * 0.1f;
            }
        }
        /* stats / temperature / other TLVs intentionally skipped. */
    }

    *frame_number = frame_no;
    return n_pts;
}

void tlv_stream_feed(TLVStream *s, const uint8_t *data, size_t n,
                     tlv_frame_cb cb, void *user) {
    if (n) {
        if (s->len + n > s->cap) {
            size_t ncap = s->cap;
            while (s->len + n > ncap) ncap *= 2;
            uint8_t *nb = realloc(s->buf, ncap);
            if (!nb) return;         /* OOM: drop this chunk, keep running */
            s->buf = nb; s->cap = ncap;
        }
        memcpy(s->buf + s->len, data, n);
        s->len += n;
    }

    static RadarPoint pts[RADAR_MAX_POINTS];
    for (;;) {
        long idx = find_magic(s->buf, s->len);
        if (idx < 0) {
            /* Keep last 7 bytes in case a magic straddles the boundary. */
            if (s->len > 7) consume(s, s->len - 7);
            if (s->len > MAX_BUFFER) consume(s, s->len / 2);
            return;
        }
        if (idx > 0) consume(s, (size_t)idx);
        if (s->len < ENVELOPE_SIZE) return;

        uint32_t total_len = rd_u32(s->buf + 8 + 4);
        if (total_len < ENVELOPE_SIZE || total_len > MAX_PACKET_LEN) {
            consume(s, 1);           /* nonsense length: skip this magic */
            continue;
        }
        if (s->len < total_len) return;   /* full packet not arrived yet */

        uint32_t fn = 0;
        int np = parse_packet(s->buf, total_len, &fn, pts, RADAR_MAX_POINTS);
        if (np < 0) { consume(s, 1); continue; }
        consume(s, total_len);
        if (cb) cb(user, fn, pts, np);
    }
}
