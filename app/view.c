/* One-pass zoom-crop + box-downscale to small 8-bit mono. See view.h. */
#include "view.h"
#include <stddef.h>

void view_shrink(const eo_frame_t *f, int zoom, uint8_t *out, int dw, int dh)
{
    if (zoom < 1) zoom = 1;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    /* centered 1/zoom crop of the source */
    int cw = f->width  / zoom;
    int ch = f->height / zoom;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int cx = (f->width  - cw) / 2;
    int cy = (f->height - ch) / 2;

    int is_y10 = (f->fmt == EO_FMT_Y10);

    for (int oy = 0; oy < dh; oy++) {
        int sy0 = cy + (int)((int64_t)oy       * ch / dh);
        int sy1 = cy + (int)((int64_t)(oy + 1) * ch / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;

        uint8_t *orow = out + (size_t)oy * dw;

        for (int ox = 0; ox < dw; ox++) {
            int sx0 = cx + (int)((int64_t)ox       * cw / dw);
            int sx1 = cx + (int)((int64_t)(ox + 1) * cw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;

            unsigned sum = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = f->data + (size_t)sy * f->stride;
                if (is_y10) {
                    for (int sx = sx0; sx < sx1; sx++) {
                        uint16_t v = (uint16_t)(row[2 * sx] | (row[2 * sx + 1] << 8));
                        sum += (unsigned)(v >> 8);   /* Y10 left-justified -> 8-bit */
                        n++;
                    }
                } else {
                    for (int sx = sx0; sx < sx1; sx++) {
                        sum += row[sx];
                        n++;
                    }
                }
            }
            orow[ox] = (uint8_t)(n ? sum / n : 0);
        }
    }
}
