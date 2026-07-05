/* render.c — reconstruct a viewable NATIVE-resolution JPEG from a recorded
 * eo_y10 frame, for replay.
 *
 * The tone map is not a new implementation: eo_tonemap() (recorder/src/
 * eo_tonemap.c) is kept byte-identical to the live EO feed's tone map, so a
 * night scene on replay looks precisely like it looked live, only at full
 * 1440x1088 instead of the downscaled display res. The only work here is
 * unpacking the recorded frame back into the 16-bit layout eo_tonemap consumes,
 * running the tone map (+ median if it was on), and JPEG.
 */
#include "recorder.h"
#include "eo_tonemap.h"
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>

typedef struct { struct jpeg_error_mgr mgr; jmp_buf env; } JErr;
static void jerr_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->env, 1); }

static int encode_gray(const uint8_t *g, int w, int h, int quality,
                       uint8_t *out, uint32_t cap, uint32_t *len)
{
    struct jpeg_compress_struct c;
    JErr err;
    unsigned char *jbuf = out;
    unsigned long jlen = 0;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jerr_exit;
    if (setjmp(err.env)) { jpeg_destroy_compress(&c); return -1; }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &jbuf, &jlen);            /* fixed caller buffer */
    c.image_width = w; c.image_height = h;
    c.input_components = 1; c.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);        /* 85 = full (pause); lower = play/bandwidth */
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < (JDIMENSION)h) {
        JSAMPROW row = (JSAMPROW)(g + (size_t)c.next_scanline * w);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    int rc = 0;
    if (jbuf != out) { if (jlen <= cap) memcpy(out, jbuf, jlen); else rc = -1; free(jbuf); }
    *len = (uint32_t)jlen;
    return rc;
}

/* Reconstruct a native JPEG. st carries the tonemap EMA across frames (pass the
 * same state on sequential frames to match the live anti-breathing; set reseed
 * on a seek/jump). median_on applies the same 3x3 median the live feed used. */
int render_native_jpeg(const uint8_t *payload, uint32_t plen, int w, int h, int mode,
                       int median_on, void *tone_state, int reseed, int quality,
                       uint8_t *out, uint32_t cap, uint32_t *outlen)
{
    uint32_t npx = (uint32_t)w * h;
    uint32_t need = mode == MODE_RAW16 ? npx * 2 : mode == MODE_Y8 ? npx : npx * 10 / 8;
    if (plen + 8 < need) return -1;

    /* left-justified 16-bit LE view (what eo_tonemap reads: (lo|hi<<8)>>6) */
    const uint8_t *y16;
    uint8_t *tmp = NULL;
    if (mode == MODE_RAW16) {
        y16 = payload;                          /* already in that layout */
    } else {
        tmp = malloc((size_t)npx * 2);
        if (!tmp) return -1;
        for (uint32_t i = 0; i < npx; i++) {
            int v10;
            if (mode == MODE_Y8) v10 = (int)payload[i] << 2;
            else { uint32_t g = i >> 2, k = i & 3; uint64_t v = 0;
                   memcpy(&v, payload + g * 5, 5); v10 = (int)((v >> (10 * k)) & 0x3ff); }
            uint16_t le = (uint16_t)(v10 << 6);
            tmp[2 * i] = (uint8_t)le; tmp[2 * i + 1] = (uint8_t)(le >> 8);
        }
        y16 = tmp;
    }

    uint16_t *sm = malloc((size_t)npx * sizeof(uint16_t));
    int *xs = malloc((size_t)(w + 1) * sizeof(int));
    uint8_t *out8 = malloc(npx);
    int rc = -1;
    if (sm && xs && out8) {
        EoToneState *st = tone_state;
        if (reseed) st->seeded = 0;
        eo_tonemap(y16, 2 * w, 0, 0, w, h, out8, w, h, st, sm, xs);
        if (median_on) { uint8_t *msc = malloc(npx); if (msc) { eo_median3(out8, w, h, msc); free(msc); } }
        rc = encode_gray(out8, w, h, quality, out, cap, outlen);
    }
    free(sm); free(xs); free(out8); free(tmp);
    return rc;
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
    EoToneState st = {0};
    uint8_t out[16384];
    uint32_t len = 0;
    int r = render_native_jpeg(raw, npx * 2, w, h, MODE_RAW16, 0, &st, 1, 85, out, sizeof out, &len);
    free(raw);
    return (r == 0 && len > 100 && out[0] == 0xFF && out[1] == 0xD8) ? 0 : -1;
}
