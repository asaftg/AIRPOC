/* http.h - tiny HTTP/1.0 + SSE server for trackerd, same shape as the detector and
 * radar daemons:
 *   GET /stream  Server-Sent-Events, one track message per publish
 *   GET /stats   health + live knobs (post-clamp)
 *   GET /ctl?..  set the live knobs (incl. engage=<tid>), reply "ok"
 *
 * This module owns the knob state so /stats echoes exactly what was applied. The
 * daemon pushes health in via http_set_*, reads knobs with http_get_knobs(), and
 * publishes each wire frame with http_publish(). Client threads only ever read the
 * latest snapshot, so a slow consumer can never stall the tracker.
 */
#ifndef TRK_HTTP_H
#define TRK_HTTP_H

#include <stddef.h>

typedef struct {
    int    engage;        /* operator-selected track id, or -1 (operator-facing) */
    double gate_base;     /* association gate base, px (bench) */
    double confirm;       /* evidence score to confirm a track (bench) */
    double coast_s;       /* seconds a confirmed track coasts on misses (bench) */
    double clutter_s;     /* translate-vs-oscillate horizon, seconds (bench) */
    int    lock;          /* 1 = allow the 60 fps engaged-target lock loop (operator) */
} TrkCtl;

int  http_start(int port);
void http_publish(const char *json, size_t len);

/* Health inputs for /stats. */
void http_set_feed(int det_connected, int eo_tap_ok, double det_fps, double out_fps);
void http_set_tracks(int live, int emitted);
void http_set_lock(int engaged_tid, int lock_on, double lock_score);
void http_set_degraded(int degraded, unsigned long err_count, const char *last_err);
void http_set_info(const char *version, double ifov_urad, int img_w, int img_h);

void http_get_ctl(TrkCtl *out);
void http_set_ctl_cb(void (*cb)(const TrkCtl *, void *user), void *user);

/* The daemon releases the lock (engaged -> -1) when its track is genuinely gone; keep the
 * knob echoed in /stats and read by /ctl in sync with that. */
void http_set_engage(int engage);

#endif /* TRK_HTTP_H */
