/* trk_parse.h - EO tracker wire (:8095 /stream) frame parser. */
#ifndef FUS_TRK_PARSE_H
#define FUS_TRK_PARSE_H

#include "core.h"
#include <stdint.h>

typedef struct {
    uint64_t frame_id;
    uint64_t t_pub_ns, t_out_ns;    /* CLOCK_MONOTONIC (diffable) */
    int      connected;
    int      engaged;               /* -1 = none */
    int      img_w, img_h;
    double   ifov_rad;              /* 0 if absent */
} TrkMeta;

int trk_parse(const char *json, FusEoTrk *out, int max, TrkMeta *meta);

#endif
