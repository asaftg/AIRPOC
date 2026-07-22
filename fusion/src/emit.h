/* emit.h - fused wire JSON serializer. */
#ifndef FUS_EMIT_H
#define FUS_EMIT_H

#include "core.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      rad_connected, trk_connected;
    uint64_t frame_id;                 /* fusion's own output counter */
    uint64_t rad_frame_id, eo_frame_id;/* last consumed upstream frames */
    int      eo_engaged;               /* eotrack "engaged" passthrough, -1 none */
    uint64_t t_out_ns;
    /* the mount trim that produced this frame's radar-sourced angles, plus the
     * observe-only estimate - on the wire so recordings self-document the
     * calibration state for offline analysis */
    double   trim_az_deg, trim_el_deg;
    double   est_az_deg, est_el_deg;
    int      est_n;
} FusHdr;

size_t fus_frame_json(char *buf, size_t cap, const FusHdr *h,
                      const FusOut *rows, int n);

#endif
