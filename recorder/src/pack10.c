/* pack10.c — Y10 (left-justified 16-bit LE) -> packed 10-bit bitstream, and
 * crc32c (ARMv8 hardware instruction, table fallback).
 *
 * Packing: pixels little-endian into a contiguous bitstream, p0 bits 0-9,
 * p1 bits 10-19, ... Four pixels -> five bytes. The kernel writes 8 bytes per
 * 4-pixel group (low 5 valid) and advances 5 — callers MUST provide 3 bytes of
 * slack after the output area. The chunk framer guarantees this.
 */
#include "recorder.h"

uint32_t pack10(const uint8_t *y10le, uint32_t n_px, uint8_t *out)
{
    const uint64_t *in = (const uint64_t *)y10le;
    uint8_t *o = out;
    uint32_t groups = n_px / 4;

    for (uint32_t g = 0; g < groups; g++) {
        uint64_t a;
        memcpy(&a, in + g, 8);                       /* 4 pixels, LE lanes  */
        uint64_t v = ((a >>  6) & 0x3ffull)
                   | ((a >> 22) & 0x3ffull) << 10
                   | ((a >> 38) & 0x3ffull) << 20
                   | ((a >> 54) & 0x3ffull) << 30;   /* 40 bits             */
        memcpy(o, &v, 8);                            /* low 5 bytes valid   */
        o += 5;
    }
    /* tail (never hit at 1440x1088: 1,566,720 % 4 == 0) */
    for (uint32_t i = groups * 4; i < n_px; i++) {
        uint16_t px16; memcpy(&px16, y10le + 2 * i, 2);
        uint16_t px = px16 >> 6;
        uint32_t bit = (i % 4) * 10;
        if (bit == 0) *o = 0;
        o[bit / 8]     |= (uint8_t)(px << (bit % 8));
        o[bit / 8 + 1]  = (uint8_t)(px >> (8 - bit % 8));
    }
    return groups * 5 + ((n_px % 4) * 10 + 7) / 8;
}

/* Y8 = top 8 of the 10 significant bits = high byte of the left-justified u16 */
uint32_t pack_y8(const uint8_t *y10le, uint32_t n_px, uint8_t *out)
{
    for (uint32_t i = 0; i < n_px; i++) out[i] = y10le[2 * i + 1];
    return n_px;
}

int pack10_selftest(void)
{
    enum { N = 64 };
    uint8_t in[N * 2], out[N * 10 / 8 + 8];
    uint16_t px[N];
    for (int i = 0; i < N; i++) {
        px[i] = (uint16_t)((i * 977 + 13) & 0x3ff);
        uint16_t v = (uint16_t)(px[i] << 6);
        in[2 * i] = (uint8_t)v; in[2 * i + 1] = (uint8_t)(v >> 8);
    }
    uint32_t n = pack10(in, N, out);
    if (n != N * 10 / 8) return -1;
    for (int i = 0; i < N; i++) {
        uint32_t bit = (uint32_t)i * 10;
        uint32_t w; memcpy(&w, out + bit / 8, 4);
        if (((w >> (bit % 8)) & 0x3ff) != px[i]) return -1;
    }
    return 0;
}

/* ---- crc32c ---- */

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
uint32_t crc32c(uint32_t seed, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t c = ~seed;
    while (len >= 8) { uint64_t v; memcpy(&v, p, 8); c = __crc32cd(c, v); p += 8; len -= 8; }
    while (len--) c = __crc32cb(c, *p++);
    return ~c;
}
#else
static uint32_t crc_tab[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
        crc_tab[i] = c;
    }
}
uint32_t crc32c(uint32_t seed, const void *buf, size_t len)
{
    if (!crc_tab[1]) crc_init();
    const uint8_t *p = buf;
    uint32_t c = ~seed;
    while (len--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
#endif
