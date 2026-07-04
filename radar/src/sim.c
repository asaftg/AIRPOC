#include "sim.h"
#include <math.h>
#include <string.h>

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put_f32(uint8_t *p, float f) {
    uint32_t u; memcpy(&u, &f, 4); put_u32(p, u);
}

/* Append one point (x,y,z + radial doppler) with `n` scattered returns
 * around it. Advances *np and writes into the point payload `pp`. Doppler
 * sign: approaching (range decreasing) -> positive, per the previewer's
 * red=approaching convention. */
static void emit_target(uint8_t *pp, int *np, int max,
                        float cx, float cy, float vx, float vy,
                        float spread, int n, float phase) {
    float r = sqrtf(cx * cx + cy * cy); if (r < 1e-3f) r = 1e-3f;
    float dop = -(cx * vx + cy * vy) / r;          /* +approaching */
    for (int i = 0; i < n && *np < max; i++) {
        float a = phase + i * 1.3f;                /* deterministic scatter */
        float jx = spread * sinf(a * 1.7f);
        float jy = spread * cosf(a * 2.3f);
        float jz = 0.3f * sinf(a * 0.9f);
        uint8_t *rec = pp + (size_t)(*np) * 16;
        put_f32(rec + 0,  cx + jx);
        put_f32(rec + 4,  cy + jy);
        put_f32(rec + 8,  jz);
        put_f32(rec + 12, dop + 0.15f * sinf(a));
        (*np)++;
    }
}

size_t sim_build_frame(uint8_t *buf, size_t cap, uint32_t frame_no, double t_s) {
    static const uint8_t MAGIC[8] = {0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07};
    float t = (float)t_s;

    /* Point payload built first so we know the count. */
    uint8_t pts[1024 * 16];
    int np = 0;

    /* Person: walks +x from -15 to +15 across y~30, at 1.2 m/s (loops). */
    float span = 30.0f, px = -15.0f + fmodf(1.2f * t, span);
    emit_target(pts, &np, 1024, px, 30.0f, 1.2f, 0.0f, 0.6f, 5, t);

    /* Vehicle: recedes in +y from 80 to 200 at 8 m/s (loops). */
    float vy_pos = 80.0f + fmodf(8.0f * t, 120.0f);
    emit_target(pts, &np, 1024, 6.0f, vy_pos, 0.0f, 8.0f, 1.5f, 10, t * 1.3f);

    /* A few static clutter returns (|doppler| < 0.2 -> drawn dim). */
    for (int i = 0; i < 3 && np < 1024; i++) {
        uint8_t *rec = pts + (size_t)np * 16;
        put_f32(rec + 0, -8.0f + 5.0f * i);
        put_f32(rec + 4, 12.0f + 3.0f * i);
        put_f32(rec + 8, 0.0f);
        put_f32(rec + 12, 0.05f);      /* ~static */
        np++;
    }

    uint32_t tlv_len   = (uint32_t)np * 16;
    uint32_t total_len = 40 + 8 + tlv_len;
    if (total_len > cap) return 0;

    memcpy(buf, MAGIC, 8);
    uint8_t *h = buf + 8;
    put_u32(h + 0,  2);            /* version   */
    put_u32(h + 4,  total_len);
    put_u32(h + 8,  0);            /* platform  */
    put_u32(h + 12, frame_no);
    put_u32(h + 16, 0);            /* timeCpuCycles */
    put_u32(h + 20, (uint32_t)np); /* numDetectedObj */
    put_u32(h + 24, 1);            /* numTLVs   */
    put_u32(h + 28, 0);            /* subFrame  */

    uint8_t *tlv = buf + 40;
    put_u32(tlv + 0, 1);           /* TLV type = DetectedPoints */
    put_u32(tlv + 4, tlv_len);
    memcpy(tlv + 8, pts, tlv_len);
    return total_len;
}
