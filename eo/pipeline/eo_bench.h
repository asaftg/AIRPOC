/* libeo — UNSTABLE bench/preview controls. NOT frozen; may change any time.
 *
 * These drive the manual exposure/gain sweep and expose telemetry for the operator
 * preview (eo/pipeline/main.c). Production consumers (the GUI) use only eo.h and
 * MUST NOT depend on anything here. Kept separate so the frozen surface stays clean.
 */
#ifndef AIRPOC_EO_BENCH_H
#define AIRPOC_EO_BENCH_H

/* Live telemetry snapshot (for the preview overlay / /stats). */
typedef struct {
    double fps;         /* delivered finished-frame rate                 */
    double sfps;        /* sensor frame rate (= 67500/vmax)              */
    double mean;        /* metered 10-bit mean                           */
    double exp_ms;      /* integration time                              */
    double duty_pct;    /* exposure/frame                                */
    int    gain;        /* 0..480 (0.1 dB/step)                          */
    int    vmax;        /* frame length                                  */
    int    ae_on;       /* 1 auto, 0 manual                              */
    int    gaincap;     /* AE gain ceiling                               */
    int    median;      /* median filter on                              */
    double focus;       /* Tenengrad sharpness (center ROI, raw)         */
    int    connected;   /* camera streaming                              */
} EoStats;

void    eo_stats(EoStats *out);       /* fill telemetry snapshot           */

/* Manual exposure/gain override. Setting gain/expms drops to manual; ae=1 -> auto. */
void    eo_set_ae(int on);
void    eo_set_gain(int gain);        /* 0..480                             */
void    eo_set_expms(double ms);      /* manual exposure, capped by the fps */
void    eo_set_gaincap(int cap);      /* AE gain ceiling                    */
void    eo_set_median(int on);
void    eo_set_fps(double fps);       /* FIXED operating fps -> caps exposure; AE never changes it */

#endif /* AIRPOC_EO_BENCH_H */
