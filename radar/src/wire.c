#include "wire.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Append helper: writes to buf[*off..cap), advances *off, never overflows. */
static void ap(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (*off >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w > 0) *off += (size_t)w;
    if (*off >= cap) *off = cap - 1;   /* saturate; caller sees truncation */
}

/* integer-dB SNR (precision matched to the sensor); null if not finite */
static void num0(char *buf, size_t cap, size_t *off, float v) {
    if (isfinite(v)) ap(buf, cap, off, "%.0f", v);
    else             ap(buf, cap, off, "null");
}

int wire_frame_json(char *buf, size_t cap, const RadarFrame *f,
                    double timestamp, double max_range_m, double fov_half_deg,
                    const char *profile) {
    size_t off = 0;
    ap(buf, cap, &off,
       "{\"connected\":true,\"frame_id\":%u,\"timestamp\":%.3f,"
       "\"profile\":\"%s\",\"max_range_m\":%.1f,\"fov_half_deg\":%.1f,"
       "\"num_points\":%d,\"num_targets\":%d,\"points\":[",
       f->frame_number, timestamp, profile ? profile : "",
       max_range_m, fov_half_deg, f->n_points, f->n_targets);

    for (int i = 0; i < f->n_points; i++) {
        const RadarPoint *p = &f->points[i];
        /* Polar is canonical - the sensor measures r/az/el (range is quantised
         * to 2.61 m bins); cartesian derives as x=r*sin(az), y=r*cos(az),
         * z=r*sin(el). Precision matches the sensor: r 2dp, az/el 1dp (best
         * bearing accuracy is ~2.6 deg), v 2dp (Doppler bin 0.108 m/s), snr
         * integer dB. Cuts the console's biggest bandwidth consumer ~50%. */
        ap(buf, cap, &off, "%s{\"r\":%.2f,\"az\":%.1f,\"el\":%.1f,\"v\":%.2f,\"snr\":",
           i ? "," : "", p->range, p->az, p->el, p->doppler);
        num0(buf, cap, &off, p->snr);
        ap(buf, cap, &off, ",\"tid\":%d}", p->tid);
    }
    ap(buf, cap, &off, "],\"targets\":[");
    for (int i = 0; i < f->n_targets; i++) {
        const RadarTarget *t = &f->targets[i];
        ap(buf, cap, &off,
           "%s{\"tid\":%d,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
           "\"vx\":%.3f,\"vy\":%.3f,\"vz\":%.3f,"
           "\"sx\":%.3f,\"sy\":%.3f,\"sz\":%.3f,"
           "\"conf\":%.3f,\"np\":%d,\"sus\":%d,\"mv_class\":%d,"
           "\"class\":\"radar_detection\"}",
           i ? "," : "", t->tid, t->x, t->y, t->z, t->vx, t->vy, t->vz,
           t->sx, t->sy, t->sz, t->conf, t->num_points, t->suspect,
           t->mv_class);
    }
    ap(buf, cap, &off, "]}");
    return (int)off;
}
