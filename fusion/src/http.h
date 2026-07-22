/* http.h - :8096 server: /stream (SSE), /stats, /ctl. */
#ifndef FUS_HTTP_H
#define FUS_HTTP_H

#include <stddef.h>

typedef struct {
    double trim_az_deg, trim_el_deg;
    double gate;
    double confirm;
    double divorce_s;
    double coast_s;
} FusCtl;

int  http_start(int port);
void http_publish(const char *json, size_t len);
void http_set_feeds(int rad_connected, int trk_connected,
                    double rad_fps, double trk_fps, double out_fps);
void http_set_tracks(int fused, int eo_only, int rad_only);
void http_set_trim_info(const char *source, double est_az_deg, double est_el_deg, int est_n);
void http_set_degraded(int degraded, unsigned long err_count, const char *last_err);
void http_get_ctl(FusCtl *out);
void http_set_ctl(const FusCtl *c);            /* seed knobs (e.g. trim from file) */
void http_set_ctl_cb(void (*cb)(const FusCtl *, void *), void *user);

#endif
