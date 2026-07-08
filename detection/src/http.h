/* http.h — tiny HTTP/1.0 + SSE server for detectiond, mirroring the radar
 * previewer's shape (radar/src/http.c): a static-free JSON API with
 *   GET /stream  Server-Sent-Events, one detection message per tick
 *   GET /stats   health + live knobs (post-clamp)
 *   GET /ctl?..  set the live knobs, reply "ok"
 *
 * This module owns the knob state (so /stats echoes exactly what was applied);
 * the daemon pushes health numbers in via the http_set_* calls and reads the
 * live knobs back with http_get_knobs(). Client threads only ever read the
 * latest published snapshot, so a slow consumer drops frames and can never
 * stall the detector.
 */
#ifndef DET_HTTP_H
#define DET_HTTP_H

#include <stddef.h>

typedef struct {
    double conf;
    int    cadence;       /* run detector every Nth captured frame */
    int    motion;        /* motion worker on/off */
    int    max_dets;
    double nms;           /* box-merge IoU threshold (lower = merge more) */
    double mot_k;         /* motion MAD threshold multiplier */
    int    mot_persist;   /* hits required in the 5-frame window */
} DetKnobs;

int  http_start(int port);

/* Publish the latest detection message (copied) for SSE /stream clients. */
void http_publish(const char *json, size_t len);

/* Health inputs for /stats. Each marks its section active. */
void http_set_tap(int connected, double fps, unsigned long gaps,
                  unsigned long drops_cum, unsigned long long frame_id);
void http_set_det(double fps, double infer_ms_p50, double infer_ms_p95,
                  double e2e_ms_p50, double e2e_ms_p95,
                  const char *model, const char *precision);
void http_set_motion(double fps, double stab_fail_pct, int candidates);

/* One-time static info for /stats. */
void http_set_info(const char *version, double ifov_urad, int img_w, int img_h);

/* Snapshot the current (clamped) knobs — the daemon reads cadence etc. live. */
void http_get_knobs(DetKnobs *out);

/* Register the /ctl handler; called with the post-clamp knobs on each hit. */
void http_set_ctl_cb(void (*cb)(const DetKnobs *, void *user), void *user);

#endif /* DET_HTTP_H */
