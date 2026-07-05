/* AIRPOC EO pipeline — on-device capture + AE + ISP (production C datapath).
 *
 * Replaces the Python bench preview on the device: V4L2 mmap capture of the
 * IMX296 Y10 stream, a flicker-free auto-exposure loop that drives the sensor,
 * a light mono ISP, and an MJPEG monitor feed. Frames are also handed to a
 * consumer callback (the detector) as linear 10-bit.
 *
 * All numeric behaviour mirrors the validated bench tool (eo/tools/) exactly.
 */
#ifndef AIRPOC_EO_PIPELINE_H
#define AIRPOC_EO_PIPELINE_H

#include <stdint.h>
#include <stddef.h>

/* ---- capture geometry (probed from the driver at runtime; these are defaults) ---- */
#define EO_DEV_DEFAULT   "/dev/video0"
#define EO_WIDTH         1440
#define EO_HEIGHT        1088

/* ---- IMX296 timing @ 60 fps (see eo/docs/DRIVER.md) ---- */
#define EO_VMAX          1125          /* frame length, lines                */
#define EO_HMAX          1100          /* line length                        */
#define EO_PIXCLK_HZ     74250000.0    /* sensor pixel clock                 */
#define EO_LINE_US       (EO_HMAX / EO_PIXCLK_HZ * 1e6)   /* 14.815 us/line  */
#define EO_FRAME_US      (EO_VMAX * EO_LINE_US)           /* 16667 us        */
#define EO_SHS1_MIN      8
#define EO_MIN_EXP_LINES 5             /* ~0.074 ms integration floor        */
/* VMAX (frame length) is now a control axis, not a constant: a longer frame =
 * lower fps = room for a longer exposure. The AE lengthens the frame (drops fps)
 * to gather light with TIME before it ever reaches for gain — the "expose, don't
 * gain" policy the seeker IMX568 bench uses. */
#define EO_VMAX_MIN      EO_VMAX       /* 1125 = 60 fps (shortest frame)     */
#define EO_VMAX_MAX      5625          /* 12 fps (longest frame; ~83 ms exp) */
#define EO_MAX_EXP_LINES (EO_VMAX_MIN - EO_SHS1_MIN)      /* 1117 (~16.5ms@60)*/
#define EO_GAIN_MIN      0
#define EO_GAIN_MAX      480           /* 0.1 dB/step (~48 dB max, = grain)  */
#define EO_GAIN_CAP      120           /* AE gain ceiling ~12 dB: lengthen the
                                        * frame first, accept dim, don't gain
                                        * into 48 dB of noise.               */
#define EO_FPS_OF_VMAX(v)  (67500.0 / (double)(v))        /* 74.25e6/1100/VMAX*/
#define EO_VMAX_OF_FPS(f)  ((int)(67500.0 / (double)(f) + 0.5))

/* ---- ISP / AE tuning ---- */
#define EO_AE_TARGET     450.0         /* target mean, 10-bit                */
#define EO_BLACK         60.0          /* black level (BLKLEVEL 0x3c)        */
#define EO_GAMMA         0.85
#define EO_MIN_SPAN      40.0          /* min p1..p99 span (10-bit counts) the
                                        * tone-map will stretch to full range;
                                        * below this the scene is flat/dim and
                                        * stretching just amplifies noise 6x+ */

/* duty = exposure_time / frame_time = exposure_lines / VMAX  (== NIR strobe duty) */
#define EO_DUTY_PCT(exp_lines, vmax)  (100.0 * (exp_lines) / (double)(vmax))
#define EO_EXP_US(exp_lines)    ((exp_lines) * EO_LINE_US)

/* ---- lens/sensor geometry for FOV (CommonLands CIL122 f=12mm, IMX296 3.45um) ---- */
#define EO_FOCAL_MM  12.0
#define EO_PIX_UM    3.45

/* ---- sensor control (i2c; SHS1 0x308d, GAIN 0x3204, REGHOLD 0x3008) ---- */
typedef struct {
    int  i2c_fd;        /* open /dev/i2c-<bus>            */
    int  addr;          /* 0x1a                          */
} Sensor;

int  sensor_open(Sensor *s);                 /* finds the *-001a bus, opens it   */
void sensor_close(Sensor *s);
/* Atomically latch VMAX(frame length) + exposure(lines) + gain(0..480) via REGHOLD.
 * SHS1 = vmax - exp_lines; a larger vmax lengthens the frame (lowers fps). */
int  sensor_apply(Sensor *s, int exp_lines, int gain, int vmax);

/* ---- auto-exposure state + step (pure function of the metered mean) ---- */
typedef struct {
    int    exp_lines;   /* current integration, lines    */
    int    gain;        /* current gain, 0..480          */
    int    vmax;        /* current frame length (fps)    */
    double mean_ema;    /* filtered metric               */
    double mean;        /* last raw metric               */
} AE;

void ae_init(AE *ae);
/* Feed the metered 10-bit mean; updates exp_lines/vmax/gain (expose-then-lengthen-
 * frame-then-gain, gain capped at gaincap so it accepts a dim frame over noise). */
void ae_update(AE *ae, double mean10, int gaincap);

/* ---- V4L2 mmap capture ---- */
typedef struct {
    int      fd;
    int      width, height, bytesperline, sizeimage;
    void   **bufs;
    size_t  *buflen;
    int      nbufs;
    uint64_t last_ts_ns;   /* dequeued buffer's CLOCK_MONOTONIC timestamp (exposure-referenced) */
    uint32_t last_seq;     /* dequeued buffer's driver frame counter (gap = driver drop)        */
} Capture;

int  cap_open(Capture *c, const char *dev, int nbufs);
void cap_close(Capture *c);
/* Blocks for one complete frame; returns a pointer valid until cap_requeue(). */
const uint8_t *cap_dqbuf(Capture *c, int *index);
void cap_requeue(Capture *c, int index);

/* ---- ISP (Y10 stride = bytesperline) ---- */
/* AE metric: mean of an 8x8 subsample, 10-bit scale. */
double isp_mean10(const uint8_t *y10, int bpl, int w, int h);
/* Focus metric: Tenengrad over the native center ROI, on 10-bit values. */
double isp_sharpness(const uint8_t *y10, int bpl, int w, int h);
/* Wire feed: crop(cx,cy,cw,ch) of the Y10 frame -> tone map -> 8-bit, native pixels
 * (crop = digital zoom; ow=cw/oh=ch keeps full resolution, no downscale). The tone
 * map is a temporally-smoothed p1/p99 stretch on the raw 10-bit (kills the blown-
 * highlight + frame-to-frame breathing the per-frame 99.5%-white version caused). */
void   isp_scale_tonemap(const uint8_t *y10, int bpl, int cx, int cy, int cw, int ch,
                         uint8_t *out8, int ow, int oh);
/* In-place edge-preserving 3x3 median on an 8-bit plane — cheap low-light grain filter. */
void   isp_median3(uint8_t *img, int w, int h);

/* ---- Wire-feed tuning. The display stream is one of four operator-selected 4:3
 * sizes (below); the DETECTOR always keeps the full-native frame. Two bandwidth
 * levers: display size (here) and operating fps (eo_set_fps). ---- */
#define EO_FEED_QUALITY  85            /* MJPEG quality (libjpeg-turbo)           */

/* ---- MJPEG monitor server for the operator preview. Consumes finished frames from
 * libeo (eo.h) and serves them; controls proxy to libeo's bench API (eo_bench.h).
 * NOTE: capture/AE/ISP/exposure state now live in libeo — not here. ---- */
int  mjpeg_start(int port);                  /* spawns the HTTP server thread    */
int  mjpeg_zoom(void);                       /* current digital zoom (1/2/4/8)   */
void mjpeg_res_dims(int *w, int *h);         /* operator-selected display size (4:3) */
const char *mjpeg_res_name(void);            /* "low"/"med"/"high"/"native"      */
void mjpeg_publish(const uint8_t *gray, int w, int h);   /* newest finished frame */

#endif /* AIRPOC_EO_PIPELINE_H */
