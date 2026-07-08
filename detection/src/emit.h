/* emit.h — build the per-tick detection JSON (the /stream SSE payload).
 *
 * Contract lives in detection/docs/INTEGRATION.md. One message per detector
 * tick, emitted even when empty (doubles as a heartbeat). Boxes carry both
 * pixels (px = [cx,cy,w,h], full-res) and real-world angle (ang = [az,el,w,h]
 * radians) via the lens IFOV — angle is what fusion needs since the camera has
 * no range. Every box is stamped with its source ("app" model / "mot" motion).
 */
#ifndef DET_EMIT_H
#define DET_EMIT_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *src;    /* "app" (model) or "mot" (motion) */
    const char *cls;    /* "human"/"vehicle"/"drone", or NULL for an unclassified mover */
    float       conf;
    int         age;    /* motion persistence age; <0 to omit */
    float       cx, cy, w, h;   /* full-res pixels */
} DetBox;

typedef struct {
    uint64_t frame_id;          /* v4l2 sequence (meta[0]) */
    uint64_t t_src_ns;          /* authoritative exposure time */
    uint64_t t_pub_ns;          /* when EO published the frame */
    uint64_t t_out_ns;          /* when we emit this message */
    int      night;             /* illuminator on */
    unsigned illum_on, illum_present, illum_power, illum_fov10;
    int      img_w, img_h;
    const char *model;          /* loaded model name, or "none" */
    double   ifov_rad;          /* radians per pixel, for the angle mapping */
    unsigned long tap_gaps;     /* cumulative tap laps */
    unsigned long drops_cum;    /* cumulative driver drops (meta[5]) */
} DetHdr;

/* Serialize into buf (NUL-terminated). Returns the byte length written (not
 * counting the NUL), or a value >= cap if it would have overflowed (truncated).
 * dets/movers may be NULL when their count is 0. */
size_t det_frame_json(char *buf, size_t cap, const DetHdr *hdr,
                      const DetBox *dets, int n_dets,
                      const DetBox *movers, int n_movers);

#endif /* DET_EMIT_H */
