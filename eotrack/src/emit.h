/* emit.h - serialise the tracker's output to the wire JSON (mirrors the radar
 * daemon's target shape so the future fusion module consumes both sensors the
 * same way). Pixels in, angles out (az +right, el +up), raw sensor frame - no
 * radar/EO calibration trim baked in (that is fusion's job). */
#ifndef TRK_EMIT_H
#define TRK_EMIT_H

#include <stdint.h>
#include <stddef.h>
#include "core.h"

typedef struct {
    uint64_t frame_id;
    uint64_t t_src_ns, t_pub_ns, t_out_ns;
    int      connected;        /* detector feed up? */
    const char *mode;          /* "stare" | "track" */
    int      engaged;          /* engaged tid or -1 */
    int      img_w, img_h;
    double   ifov_rad;
} TrkHdr;

/* Write the full /stream JSON frame. Returns bytes written (<= cap-1, NUL-set). */
size_t trk_frame_json(char *buf, size_t cap, const TrkHdr *h,
                      const TrkOut *tracks, int n);

#endif /* TRK_EMIT_H */
