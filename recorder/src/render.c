/* render.c — reconstruct a viewable NATIVE-resolution JPEG from a recorded
 * eo_y10 frame, for replay. The display channel was whatever low res the
 * operator selected; native replay decodes the raw sensor frame at full
 * 1440x1088 so no detail is lost.
 *
 * Raw Y10 is linear, so a night scene occupies a small slice of the range —
 * a flat >>2 would look black. We apply the same idea as the live ISP: a
 * per-frame percentile stretch (p1..p99, subsampled) + gamma, then libjpeg
 * grayscale encode. This is a replay rendering choice, self-contained here —
 * it does not touch the EO module's ISP.
 */
#include "recorder.h"
#include <stdlib.h>
#include <math.h>
#include <setjmp.h>
#include <jpeglib.h>

#define TONE_GAMMA 0.85
#define HIST_BINS  1024

/* decode one pixel's 10-bit value by channel encoding */
static inline int decode_px(const uint8_t *p, uint32_t i, int mode)
{
    if (mode == MODE_RAW16) { uint16_t v; memcpy(&v, p + 2 * i, 2); return v >> 6; }
    if (mode == MODE_Y8)    return (int)p[i] << 2;
    /* MODE_Y10P: 4 px per 5 bytes, LSB-first */
    uint32_t grp = i >> 2, k = i & 3;
    uint64_t v = 0;
    memcpy(&v, p + grp * 5, 5);
    return (int)((v >> (10 * k)) & 0x3ff);
}

typedef struct { struct jpeg_error_mgr mgr; jmp_buf env; } JErr;
static void jerr_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->env, 1); }

int render_y10_to_jpeg(const uint8_t *payload, uint32_t plen, int w, int h, int mode,
                       uint8_t *out, uint32_t cap, uint32_t *outlen)
{
    uint32_t npx = (uint32_t)w * h;
    /* sanity: payload must be able to hold npx pixels in this encoding */
    uint32_t need = mode == MODE_RAW16 ? npx * 2 : mode == MODE_Y8 ? npx : npx * 10 / 8;
    if (plen + 8 < need) return -1;

    uint8_t *gray = malloc(npx);         /* set once, before setjmp; freed on all paths */
    if (!gray) return -1;

    /* percentile histogram, subsampled every 4th pixel */
    uint32_t hist[HIST_BINS] = { 0 };
    uint32_t nsamp = 0;
    for (uint32_t i = 0; i < npx; i += 4) { hist[decode_px(payload, i, mode)]++; nsamp++; }
    uint32_t lo_c = nsamp / 100, hi_c = nsamp - nsamp / 100;
    int lo = 0, hi = HIST_BINS - 1;
    uint32_t acc = 0;
    for (int b = 0; b < HIST_BINS; b++) { acc += hist[b]; if (acc >= lo_c) { lo = b; break; } }
    acc = 0;
    for (int b = 0; b < HIST_BINS; b++) { acc += hist[b]; if (acc >= hi_c) { hi = b; break; } }
    if (hi <= lo) { lo = 0; hi = HIST_BINS - 1; }

    /* LUT: stretch [lo,hi] -> [0,255] with gamma */
    uint8_t lut[HIST_BINS];
    double span = hi - lo;
    for (int v = 0; v < HIST_BINS; v++) {
        double n = v <= lo ? 0.0 : v >= hi ? 1.0 : (v - lo) / span;
        lut[v] = (uint8_t)(pow(n, TONE_GAMMA) * 255.0 + 0.5);
    }
    for (uint32_t i = 0; i < npx; i++) gray[i] = lut[decode_px(payload, i, mode)];

    /* encode grayscale JPEG into the caller's buffer */
    struct jpeg_compress_struct c;
    JErr err;
    unsigned long jlen = 0;
    unsigned char *jbuf = out;
    unsigned long jcap = cap;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jerr_exit;
    if (setjmp(err.env)) { jpeg_destroy_compress(&c); free(gray); return -1; }
    jpeg_create_compress(&c);
    /* encode straight into the fixed caller buffer (no libjpeg malloc/realloc) */
    jpeg_mem_dest(&c, &jbuf, &jlen);
    c.image_width = w; c.image_height = h;
    c.input_components = 1; c.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 85, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < (JDIMENSION)h) {
        JSAMPROW row = gray + (size_t)c.next_scanline * w;
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(gray);

    if (jbuf != out) {                       /* libjpeg grew its own buffer */
        if (jlen <= jcap) memcpy(out, jbuf, jlen);
        free(jbuf);
        if (jlen > jcap) return -1;
    }
    *outlen = (uint32_t)jlen;
    return 0;
}

int render_selftest(void)
{
    int w = 64, h = 48;
    uint32_t npx = (uint32_t)w * h;
    uint8_t *raw = malloc(npx * 2);
    for (uint32_t i = 0; i < npx; i++) {
        uint16_t v10 = (uint16_t)((i * 7) & 0x3ff);
        uint16_t le = (uint16_t)(v10 << 6);
        raw[2 * i] = (uint8_t)le; raw[2 * i + 1] = (uint8_t)(le >> 8);
    }
    uint8_t out[16384];
    uint32_t len = 0;
    int r = render_y10_to_jpeg(raw, npx * 2, w, h, MODE_RAW16, out, sizeof out, &len);
    free(raw);
    /* JPEG SOI marker + non-trivial length */
    return (r == 0 && len > 100 && out[0] == 0xFF && out[1] == 0xD8) ? 0 : -1;
}
