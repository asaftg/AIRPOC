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
#define EO_SHS1_MAX      1100
#define EO_MAX_EXP_LINES (EO_VMAX - EO_SHS1_MIN)          /* 1117            */
#define EO_MIN_EXP_LINES (EO_VMAX - EO_SHS1_MAX)          /* 25              */
#define EO_GAIN_MIN      0
#define EO_GAIN_MAX      480           /* 0.1 dB / step                      */

/* ---- ISP / AE tuning (matches the bench tool) ---- */
#define EO_AE_TARGET     450.0         /* target mean, 10-bit                */
#define EO_BLACK         60.0          /* black level (BLKLEVEL 0x3c)        */
#define EO_GAMMA         0.85

/* duty = exposure_time / frame_time = exposure_lines / VMAX  (== NIR strobe duty) */
#define EO_DUTY_PCT(exp_lines)  (100.0 * (exp_lines) / EO_VMAX)
#define EO_EXP_US(exp_lines)    ((exp_lines) * EO_LINE_US)

/* ---- sensor control (i2c; SHS1 0x308d, GAIN 0x3204, REGHOLD 0x3008) ---- */
typedef struct {
    int  i2c_fd;        /* open /dev/i2c-<bus>            */
    int  addr;          /* 0x1a                          */
} Sensor;

int  sensor_open(Sensor *s);                 /* finds the *-001a bus, opens it   */
void sensor_close(Sensor *s);
/* Atomically latch exposure (in lines) + gain (0..480) via REGHOLD. */
int  sensor_apply(Sensor *s, int exp_lines, int gain);

/* ---- auto-exposure state + step (pure function of the metered mean) ---- */
typedef struct {
    int    exp_lines;   /* current integration, lines    */
    int    gain;        /* current gain, 0..480          */
    double mean_ema;    /* filtered metric               */
    double mean;        /* last raw metric               */
} AE;

void ae_init(AE *ae);
/* Feed the metered 10-bit mean; updates exp_lines/gain (flicker-free law). */
void ae_update(AE *ae, double mean10);

/* ---- V4L2 mmap capture ---- */
typedef struct {
    int      fd;
    int      width, height, bytesperline, sizeimage;
    void   **bufs;
    size_t  *buflen;
    int      nbufs;
} Capture;

int  cap_open(Capture *c, const char *dev, int nbufs);
void cap_close(Capture *c);
/* Blocks for one complete frame; returns a pointer valid until cap_requeue(). */
const uint8_t *cap_dqbuf(Capture *c, int *index);
void cap_requeue(Capture *c, int index);

/* ---- ISP: Y10 (stride bytesperline) -> 8-bit tone-mapped mono ---- */
/* Meter: mean of a subsample, 10-bit scale (for AE). */
double isp_mean10(const uint8_t *y10, int bpl, int w, int h);
/* Tone-map a Y10 frame to an 8-bit grayscale buffer (w*h). */
void   isp_tonemap(const uint8_t *y10, int bpl, int w, int h, uint8_t *out8);

/* ---- MJPEG monitor server (drop-in for the bench preview) ---- */
int  mjpeg_start(int port);                  /* spawns the HTTP server thread    */
void mjpeg_publish(const uint8_t *gray, int w, int h,   /* latest frame + overlay */
                   double fps, double mean, int exp_lines, int gain);

#endif /* AIRPOC_EO_PIPELINE_H */
