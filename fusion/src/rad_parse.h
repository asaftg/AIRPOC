/* rad_parse.h - radar wire (:8092 /stream) frame parser. */
#ifndef FUS_RAD_PARSE_H
#define FUS_RAD_PARSE_H

#include "core.h"
#include <stdint.h>

typedef struct {
    uint64_t frame_id;      /* chip frameNumber */
    int      connected;
    double   timestamp_s;   /* host CLOCK_MONOTONIC seconds (radar wire) */
} RadMeta;

/* Parses targets[] only - points[] is skipped entirely (the scan starts at
 * the "targets" key, so the large point cloud costs nothing). */
int rad_parse(const char *json, FusRadTgt *out, int max, RadMeta *meta);

#endif
