/* Flicker-free auto-exposure, "expose don't gain" (mirrors the seeker IMX568 bench).
 * Filter the metric (EMA), act in the log-brightness domain with damping + a slew cap
 * and a wide deadband, then realize the target brightness on a THREE-rung ladder:
 *   1. exposure time, up to the 60 fps frame (16.5 ms)
 *   2. lengthen the frame (raise VMAX -> lower fps, down to EO_VMAX_MAX) for MORE
 *      exposure time — gathering light optically, no added noise
 *   3. only then analog gain, HARD-CAPPED at gaincap (~12 dB), accepting a dim frame
 *      rather than gaining into 48 dB of grain. The tone-map lifts the dim frame.
 * No fixed additive steps (those pump near a bright source). */
#include "pipeline.h"
#include <math.h>

/* register step is 0.1 dB -> linear brightness factor */
static double gain_lin(int g) { return pow(10.0, g * 0.1 / 20.0); }

void ae_init(AE *ae)
{
    ae->exp_lines = 725;               /* ~10.7 ms; shs1 = VMAX - 725 = 400 */
    ae->gain      = 40;
    ae->vmax      = EO_VMAX_MIN;       /* start at 60 fps                   */
    ae->mean_ema  = EO_AE_TARGET;
    ae->mean      = 0.0;
}

void ae_update(AE *ae, double mean10, int gaincap)
{
    if (gaincap < EO_GAIN_MIN) gaincap = EO_GAIN_MIN;
    if (gaincap > EO_GAIN_MAX) gaincap = EO_GAIN_MAX;

    ae->mean = mean10;
    ae->mean_ema = 0.6 * ae->mean_ema + 0.4 * mean10;

    double ratio = EO_AE_TARGET / (ae->mean_ema > 1.0 ? ae->mean_ema : 1.0);
    if (ratio > 0.90 && ratio < 1.11) return;          /* +/-10% deadband */

    double factor = sqrt(ratio);                       /* damping: half the error */
    if (factor > 1.5)   factor = 1.5;                  /* slew cap */
    if (factor < 0.667) factor = 0.667;

    /* current brightness = exposure_lines * gain; VMAX only gates how many lines fit */
    double B = ae->exp_lines * gain_lin(ae->gain) * factor;   /* target brightness */

    /* Rung 1+2: spend exposure, extending the frame (dropping fps) up to EO_VMAX_MAX
     * before touching gain. */
    double exp = B;
    double exp_ceiling = EO_VMAX_MAX - EO_SHS1_MIN;    /* longest integration allowed */
    if (exp > exp_ceiling) exp = exp_ceiling;
    if (exp < EO_MIN_EXP_LINES) exp = EO_MIN_EXP_LINES;

    int vmax = EO_VMAX_MIN;                            /* stay at 60 fps while there's light */
    if (exp > EO_VMAX_MIN - EO_SHS1_MIN) {             /* need a longer frame */
        vmax = (int)lround(exp) + EO_SHS1_MIN;
        if (vmax > EO_VMAX_MAX) vmax = EO_VMAX_MAX;
    }

    /* Rung 3: whatever brightness exposure couldn't reach -> gain, capped low. */
    double g_lin_needed = B / exp;
    if (g_lin_needed < 1.0) g_lin_needed = 1.0;
    int g = (int)lround(20.0 * log10(g_lin_needed) / 0.1);
    if (g < EO_GAIN_MIN) g = EO_GAIN_MIN;
    if (g > gaincap)     g = gaincap;                  /* accept dim past here */

    ae->exp_lines = (int)lround(exp);
    ae->vmax      = vmax;
    ae->gain      = g;
}
