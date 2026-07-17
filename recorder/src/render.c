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

/* The operator's zoom crop (verbatim of eo/pipeline/main.c:zoom_crop_43) — the
 * region the live view metered its tone map on. We render the FULL native frame
 * but meter the tone map on this crop, so a night frame's dark periphery doesn't
 * over-stretch and blow out the illuminated center. mz<=1 with no dims => whole. */
static void native_meter_rect(int sw, int sh, int mz, int mdw, int mdh,
                              int *mx, int *my, int *mw, int *mh)
{
    if (mz <= 1 || mdw <= 0 || mdh <= 0) { *mx = *my = *mw = *mh = 0; return; }  /* whole frame */
    int rw = sw / mz, rh = sh / mz;
    int rh43 = rw * mdh / mdw;
    if (rh43 > rh) { rh43 = rh; rw = rh43 * mdw / mdh; }
    *mw = rw; *mh = rh43; *mx = (sw - rw) / 2; *my = (sh - rh43) / 2;
}

/* Reconstruct the native 8-bit frame via the shared tone map. st carries the
 * EMA across frames (same state on sequential frames = live anti-breathing;
 * reseed on a seek/jump). median_on applies the same 3x3 median the live feed
 * used. meter_z/meter_dw/meter_dh = the operator's recorded zoom + display dims;
 * the tone map is metered on that crop (0/0/0 = whole frame). out8 holds w*h. */
int render_native_gray8(const uint8_t *payload, uint32_t plen, int w, int h, int mode,
                        int median_on, void *tone_state, int reseed, uint8_t *out8,
                        int meter_z, int meter_dw, int meter_dh)
{
    if (w < 16 || w > 8192 || h < 16 || h > 8192) return -1;   /* reject corrupt geometry before the size math overflows */
    uint32_t npx = (uint32_t)w * h;
    uint32_t need = mode == MODE_RAW16 ? npx * 2 : mode == MODE_Y8 ? npx : npx * 10 / 8;
    if (plen < need) return -1;                                 /* require the full frame — a short record would read OOB in the unpack */

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
    int rc = -1;
    if (sm && xs) {
        EoToneState *st = tone_state;
        if (reseed) st->seeded = 0;
        int mx, my, mw, mh;
        native_meter_rect(w, h, meter_z, meter_dw, meter_dh, &mx, &my, &mw, &mh);
        eo_tonemap(y16, 2 * w, 0, 0, w, h, out8, w, h, st, sm, xs, mx, my, mw, mh);
        if (median_on) { uint8_t *msc = malloc(npx); if (msc) { eo_median3(out8, w, h, msc); free(msc); } }
        rc = 0;
    }
    free(sm); free(xs); free(tmp);
    return rc;
}

/* Native JPEG for one frame (pause/step/scrub stills). */
int render_native_jpeg(const uint8_t *payload, uint32_t plen, int w, int h, int mode,
                       int median_on, void *tone_state, int reseed, int quality,
                       uint8_t *out, uint32_t cap, uint32_t *outlen,
                       int meter_z, int meter_dw, int meter_dh)
{
    uint32_t npx = (uint32_t)w * h;
    uint8_t *out8 = malloc(npx);
    if (!out8) return -1;
    int rc = render_native_gray8(payload, plen, w, h, mode, median_on, tone_state, reseed, out8,
                                 meter_z, meter_dw, meter_dh);
    if (rc == 0) rc = encode_gray(out8, w, h, quality, out, cap, outlen);
    free(out8);
    return rc;
}

/* Decode a grayscale JPEG into out (>= its w*h). Returns 0 + sets ow/oh. */
int render_decode_jpeg_gray(const uint8_t *jpg, uint32_t len, uint8_t *out, uint32_t cap,
                            int *ow, int *oh)
{
    struct jpeg_decompress_struct d;
    JErr err;
    d.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jerr_exit;
    if (setjmp(err.env)) { jpeg_destroy_decompress(&d); return -1; }
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, (unsigned char *)jpg, len);
    if (jpeg_read_header(&d, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&d); return -1; }
    d.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&d);
    if ((uint32_t)d.output_width * d.output_height > cap) { jpeg_destroy_decompress(&d); return -1; }
    *ow = d.output_width; *oh = d.output_height;
    while (d.output_scanline < d.output_height) {
        JSAMPROW r = out + (size_t)d.output_scanline * d.output_width;
        jpeg_read_scanlines(&d, &r, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
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
    EoToneState st = {0};
    uint8_t out[16384];
    uint32_t len = 0;
    int r = render_native_jpeg(raw, npx * 2, w, h, MODE_RAW16, 0, &st, 1, 85, out, sizeof out, &len, 0, 0, 0);
    free(raw);
    return (r == 0 && len > 100 && out[0] == 0xFF && out[1] == 0xD8) ? 0 : -1;
}
