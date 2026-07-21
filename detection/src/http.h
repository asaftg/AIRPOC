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
 *
 * Four of these knobs are operator-facing and belong in the GUI: `conf`,
 * `temporal`, `tbd_frames` and `tbd_lo`. Everything else is bench tuning,
 * reached with curl. The mot_* knobs belong to the FROZEN motion worker
 * (see config.h).
 */
#ifndef DET_HTTP_H
#define DET_HTTP_H

#include <stddef.h>

typedef struct {
    double conf;          /* "strong" tier: emitted immediately, no integration */
    int    cadence;       /* run detector every Nth captured frame */
    int    max_dets;
    double nms;           /* box-merge IoU threshold (lower = merge more) */

    /* temporal integration (track-before-detect) — see temporal.h */
    int    temporal;      /* on/off; operator-facing ("EO temporal" button) */
    int    tbd_frames;    /* frames integrated before a floor-level target is reported;
                             operator-facing */
    double tbd_lo;        /* candidate floor + evidence reference; operator-facing */
    double tbd_decay;     /* score subtracted per missed tick; bench only */
    int    tbd_max_miss;  /* consecutive missed ticks before a track is dropped; bench only */

    /* motion worker — FROZEN, off by default (config.h) */
    int    motion;
    double mot_k;         /* motion MAD threshold multiplier */
    double mot_window_s;  /* rolling-background window (seconds) */
    int    mot_persist;   /* confirmation strength 1..5 */
    int    mot_down;      /* motion spatial downscale (1 = native) */
    int    mot_method;    /* 0 = background-subtraction, 1 = frame-difference */
    double mot_baseline_s;/* frame-diff baseline (seconds back) */
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
/* Temporal integrator health: tracks carried, and how many of the boxes emitted on
 * the last tick were promoted by integration rather than passed by `conf`. */
void http_set_temporal(int live_tracks, int promoted_last);

/* One-time static info for /stats. */
void http_set_info(const char *version, double ifov_urad, int img_w, int img_h);

/* Snapshot the current (clamped) knobs — the daemon reads cadence etc. live. */
void http_get_knobs(DetKnobs *out);

/* Register the /ctl handler; called with the post-clamp knobs on each hit. */
void http_set_ctl_cb(void (*cb)(const DetKnobs *, void *user), void *user);

#endif /* DET_HTTP_H */
