/* Tiny HTTP/1.0 + Server-Sent-Events server for the radar previewer.
 * Mirrors the EO pipeline's monitor shape (static page + /stats + /ctl),
 * but streams JSON point-cloud frames over SSE (/stream) instead of MJPEG
 * — SSE is a fraction of the code of a WebSocket and is the right push
 * channel for small structured frames.
 *
 * The daemon thread owns the UART->parse->cluster pipeline and publishes a
 * finished frame snapshot here; client threads only ever read the latest
 * snapshot, so a slow browser drops *display* frames and can never stall
 * the drop-free parse path. */
#ifndef AIRPOC_HTTP_H
#define AIRPOC_HTTP_H

#include <stddef.h>

int  http_start(int port, const char *webroot);

/* Publish the latest frame JSON (copied) for SSE clients. */
void http_publish(const char *json, size_t len);

/* Publish operator stats for /stats. */
void http_set_stats(double fps, unsigned long drops, int n_points,
                    int n_targets, int connected, const char *profile,
                    double max_range_m, double fov_half_deg);

/* Register the handler for GET /ctl?eps=&minpts=&speed=&snrmin=. Called with
 * the parsed (clamped) values on each /ctl hit; `user` is passed back verbatim. */
void http_set_ctl_cb(void (*cb)(double eps_m, int min_pts, double speed_min,
                                double snr_min, void *user), void *user);

#endif /* AIRPOC_HTTP_H */
