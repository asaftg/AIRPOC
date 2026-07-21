/* det_feed.h - consumes the detector's SSE /stream (:8094) on a background thread,
 * parses each frame's boxes into TrkDet[], and hands them to a callback. Reconnects
 * with backoff; a missing detector is not an error, just connected=0. The parser is
 * tolerant of the wire evolving (fields are conditionally emitted - it reads what is
 * present and ignores the rest), so a detector upgrade does not break it. */
#ifndef TRK_DET_FEED_H
#define TRK_DET_FEED_H

#include <stdint.h>
#include "core.h"

typedef struct {
    uint64_t frame_id;
    uint64_t t_src_ns, t_pub_ns, t_out_ns;
    double   ifov_rad;       /* 0 if absent -> caller keeps its default */
    int      have_ifov;
} DetMeta;

/* Called for every parsed detector frame (dets + movers merged into one list, since
 * the detector already guarantees one box per target). Runs on the feed thread. */
typedef void (*DetFrameCb)(const TrkDet *dets, int n, const DetMeta *m, void *user);

/* Parse one detector /stream JSON payload. Returns box count (<= max). */
int det_parse(const char *json, TrkDet *out, int max, DetMeta *meta);

/* Start the SSE consumer thread. host/port = detector daemon. */
int  det_feed_start(const char *host, int port, DetFrameCb cb, void *user);
/* 1 if a frame arrived within the last `stale_ns`, else 0. */
int  det_feed_connected(uint64_t stale_ns);

#endif /* TRK_DET_FEED_H */
