/* EO frame handoff — the single contract between the EO channel and the GUI.
 *
 * The EO camera is the Waveshare IMX296 (Sony IMX296, mono global-shutter, visible —
 * not thermal). The EO channel owns capture/AE/ISP and implements this; the GUI only
 * READS the most-recent finished frame, never copies on the capture path, and adds no
 * load to it. If the camera isn't present the GUI shows "EO · NOT CONNECTED" (no
 * synthetic imagery) — `eo_connected()` gates that.
 *
 * Zero-copy contract: eo_get_latest() returns a BORROWED pointer into an EO-owned
 * buffer. It stays valid until the next eo_get_latest() by the same consumer — the
 * EO channel keeps enough buffers that a read finishing within ~one frame period is
 * always safe (the GUI's shrink+encode is a few ms, well under that).
 */
#ifndef AIRPOC_EO_FRAME_H
#define AIRPOC_EO_FRAME_H

#include <stdint.h>

typedef enum {
    EO_FMT_GRAY8 = 0,   /* 8-bit display-ready mono, stride bytes/row            */
    EO_FMT_Y10   = 1     /* Y10 left-justified in 16-bit LE words, stride bytes/row */
} eo_fmt_t;

typedef struct {
    const uint8_t *data;    /* borrowed EO-owned pixels (see zero-copy contract) */
    uint64_t       seq;     /* monotonic; unchanged seq => same frame            */
    int            width;
    int            height;
    int            stride;   /* bytes per row                                    */
    eo_fmt_t       fmt;
    double         t_capture; /* CLOCK_MONOTONIC seconds at capture (frame age)  */
} eo_frame_t;

/* Lifecycle. eo_start() returns 0 on success (camera opened); non-zero if absent —
 * the app keeps running and the GUI shows NOT CONNECTED. dev is the capture device. */
int  eo_start(const char *dev);
void eo_stop(void);

/* Cheap read of the most-recent finished frame. Returns 1 and fills *out if a frame
 * is available, else 0. Must not add load to the capture path. */
int  eo_get_latest(eo_frame_t *out);

/* 1 if the camera is present and streaming; 0 if absent (GUI shows NOT CONNECTED). */
int  eo_connected(void);

/* Optics for camera-FOV math (from the IMX296 sensor + lens). */
double eo_focal_mm(void);
double eo_pixel_um(void);

#endif /* AIRPOC_EO_FRAME_H */
