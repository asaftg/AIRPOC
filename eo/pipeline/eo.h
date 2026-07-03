/* libeo — FROZEN public EO handoff. v1.
 *
 * The EO module owns the camera and the entire capture -> auto-exposure -> ISP
 * datapath. A consumer (the GUI, a recorder, a detector front-end) links libeo.a,
 * calls eo_start() once, and pulls finished, display-ready frames with eo_latest().
 * It does NOT open /dev/video0, does NOT run AE/sensor/ISP — that is all behind
 * this surface, and its internals change freely without touching this header.
 *
 * CONTRACT (do not break — bump EO_API_VERSION if it ever must change):
 *   - Six functions below. Nothing else here is load-bearing for a consumer.
 *   - eo_latest() hands the newest FINISHED 8-bit mono frame (already tone-mapped,
 *     denoised, full sensor resolution). Pointer is stable until the NEXT eo_latest()
 *     call on the same thread (triple-buffered, no copy).
 *   - The camera is single-owner V4L2: exactly ONE process may eo_start() at a time.
 *
 * See INTEGRATION.md.
 */
#ifndef AIRPOC_EO_H
#define AIRPOC_EO_H

#include <stdint.h>

#define EO_API_VERSION 1

#define EO_FMT_GRAY8 0          /* fmt value: 8-bit mono, 1 byte/pixel */

/* Open the camera and start capture+AE+ISP. dev = "/dev/video0" (NULL -> default).
 * Returns 0 on success, <0 on failure (camera busy, no sensor, etc.). Idempotent:
 * a second call while running is a no-op returning 0. */
int  eo_start(const char *dev);

/* Stop the datapath and release the camera. Safe to call if not started. */
void eo_stop(void);

/* Newest finished frame. On success sets *buf (valid until the next eo_latest call
 * on this thread), *seq (monotonic frame id — unchanged means no new frame),
 * w and h (pixels), stride (bytes/row, == w for GRAY8), fmt (EO_FMT_GRAY8).
 * Returns 1 if a frame is available, 0 if none yet (camera warming up). Any of the
 * out-params may be NULL if not needed. */
int  eo_latest(const uint8_t **buf, uint64_t *seq,
               int *w, int *h, int *stride, int *fmt);

/* 1 once the camera is streaming, else 0. */
int    eo_connected(void);

/* Optics constants for FOV / geometry math (lens + sensor pixel pitch). */
double eo_focal_mm(void);
double eo_pixel_um(void);

#endif /* AIRPOC_EO_H */
