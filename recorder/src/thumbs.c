/* thumbs.c — 8 evenly-spread stills per saved session, decoded from the
 * recorded display JPEGs with libjpeg's DCT downscale (scale_denom) and
 * re-encoded small (~<=192 px wide, q70). Runs at save time / lazily — never
 * during recording's hot path.
 */
#define _GNU_SOURCE
#include "recorder.h"
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <jpeglib.h>

typedef struct { struct jpeg_error_mgr mgr; jmp_buf env; } JErr;

static void jerr_exit(j_common_ptr c)
{
    JErr *e = (JErr *)c->err;
    longjmp(e->env, 1);
}

static int thumb_one(const uint8_t *jpg, uint32_t jlen, const char *outpath)
{
    struct jpeg_decompress_struct d;
    struct jpeg_compress_struct comp;
    JErr derr, cerr;
    uint8_t *volatile pixels = NULL;     /* survive longjmp */
    FILE *volatile out = NULL;
    volatile int ok = -1;

    d.err = jpeg_std_error(&derr.mgr);
    derr.mgr.error_exit = jerr_exit;
    if (setjmp(derr.env)) goto done_d;
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, (unsigned char *)jpg, jlen);
    if (jpeg_read_header(&d, TRUE) != JPEG_HEADER_OK) goto done_d;

    d.scale_num = 1;
    d.scale_denom = 1;
    while (d.scale_denom < 8 && d.image_width / (d.scale_denom * 2) >= 180)
        d.scale_denom *= 2;
    d.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&d);

    uint32_t w = d.output_width, h = d.output_height;
    pixels = malloc((size_t)w * h);
    if (!pixels) goto done_d;
    while (d.output_scanline < h) {
        JSAMPROW row = pixels + (size_t)d.output_scanline * w;
        jpeg_read_scanlines(&d, &row, 1);
    }
    jpeg_finish_decompress(&d);

    out = fopen(outpath, "wb");
    if (!out) goto done_d;
    comp.err = jpeg_std_error(&cerr.mgr);
    cerr.mgr.error_exit = jerr_exit;
    if (setjmp(cerr.env)) goto done_c;
    jpeg_create_compress(&comp);
    jpeg_stdio_dest(&comp, out);
    comp.image_width = w;
    comp.image_height = h;
    comp.input_components = 1;
    comp.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&comp);
    jpeg_set_quality(&comp, 70, TRUE);
    jpeg_start_compress(&comp, TRUE);
    while (comp.next_scanline < h) {
        JSAMPROW row = pixels + (size_t)comp.next_scanline * w;
        jpeg_write_scanlines(&comp, &row, 1);
    }
    jpeg_finish_compress(&comp);
    ok = 0;
done_c:
    jpeg_destroy_compress(&comp);
done_d:
    jpeg_destroy_decompress(&d);
    if (out) fclose(out);
    free(pixels);
    if (ok != 0) unlink(outpath);
    return ok;
}

int thumbs_generate(const char *dir)
{
    char ipath[720];
    snprintf(ipath, sizeof ipath, "%s/eo_jpeg/index.bin", dir);
    FILE *f = fopen(ipath, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long rows = ftell(f) / (long)sizeof(AirecIdxRow);
    if (rows <= 0) { fclose(f); return -1; }

    char tdir[720];
    snprintf(tdir, sizeof tdir, "%s/thumbs", dir);
    mkdir(tdir, 0755);

    uint8_t *jpg = malloc(1 << 20);
    int made = 0;
    for (int k = 0; k < 8; k++) {
        long i = rows <= 8 ? (k < rows ? k : rows - 1)
                           : (long)((rows - 1) * (double)k / 7.0);
        AirecIdxRow row;
        fseek(f, i * (long)sizeof row, SEEK_SET);
        if (fread(&row, sizeof row, 1, f) != 1) continue;
        if (row.payload_len > (1u << 20)) continue;

        char spath[720];
        snprintf(spath, sizeof spath, "%s/eo_jpeg/data.%05u.airec", dir, row.segment_no);
        FILE *sf = fopen(spath, "rb");
        if (!sf) continue;
        AirecRecHdr h;
        int got = fseek(sf, (long)row.offset, SEEK_SET) == 0 &&
                  fread(&h, sizeof h, 1, sf) == 1 &&
                  h.magic == AIREC_REC_MAGIC &&
                  fread(jpg, 1, row.payload_len, sf) == row.payload_len;
        fclose(sf);
        if (!got) continue;

        char opath[760];
        snprintf(opath, sizeof opath, "%s/thumbs/%d.jpg", dir, k);
        if (thumb_one(jpg, row.payload_len, opath) == 0) made++;
    }
    free(jpg);
    fclose(f);
    return made > 0 ? 0 : -1;
}
