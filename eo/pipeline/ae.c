/* Flicker-free auto-exposure, "expose don't gain" (mirrors the seeker IMX568 bench).
 * Filter the metric (EMA); when the brightness is off-target, correct it in the
 * log-brightness domain with damping + a slew cap; when it's on-target, HOLD the
 * brightness. Either way, realize that brightness on a THREE-rung ladder EVERY tick:
 *   1. exposure time, up to the 60 fps frame (16.5 ms)
 *   2. lengthen the frame (raise VMAX -> lower fps) for MORE exposure time — light
 *      gathered optically, no added noise
 *   3. only then analog gain, HARD-CAPPED at gaincap.
 * Because the ladder re-runs every tick (not only when off-target), gain is
 * continuously MINIMIZED: any gain that exposure/frame-length could carry instead is
 * shifted off gain at constant brightness. Gain is never left sitting where it isn't
 * strictly needed — it only rises once exposure is fully maxed (rung 3). */
#include "pipeline.h"
#include <math.h>

/* register step is 0.1 dB -> linear brightness factor */
static double gain_lin(int g) { return pow(10.0, g * 0.1 / 20.0); }

void ae_init(AE *ae)
{
    ae->exp_lines = 725;               /* ~10.7 ms; shs1 = VMAX - 725 = 400 */
    ae->gain      = 0;                 /* start with NO gain                */
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
    double factor;
    if (ratio > 0.90 && ratio < 1.11) {
        factor = 1.0;                                  /* on target: hold brightness ... */
    } else {                                           /* ... else correct it, damped     */
        factor = sqrt(ratio);
        if (factor > 1.5)   factor = 1.5;              /* slew cap */
        if (factor < 0.667) factor = 0.667;
    }

    /* target brightness = exposure_lines * gain (VMAX only gates how many lines fit) */
    double B = ae->exp_lines * gain_lin(ae->gain) * factor;

    /* Rung 1+2: spend exposure, extending the frame (dropping fps) up to EO_VMAX_MAX
     * before touching gain. Re-run every tick, so leftover gain is respent here. */
    double exp = B;
    double exp_ceiling = EO_VMAX_MAX - EO_SHS1_MIN;    /* longest integration allowed */
    if (exp > exp_ceiling) exp = exp_ceiling;
    if (exp < EO_MIN_EXP_LINES) exp = EO_MIN_EXP_LINES;

    int vmax = EO_VMAX_MIN;                            /* stay at 60 fps while there's light */
    if (exp > EO_VMAX_MIN - EO_SHS1_MIN) {             /* need a longer frame */
        vmax = (int)lround(exp) + EO_SHS1_MIN;
        if (vmax > EO_VMAX_MAX) vmax = EO_VMAX_MAX;
    }

    /* Rung 3: only the brightness exposure couldn't reach -> gain, capped low. */
    double g_lin_needed = B / exp;
    if (g_lin_needed < 1.0) g_lin_needed = 1.0;
    int g = (int)lround(20.0 * log10(g_lin_needed) / 0.1);
    if (g < EO_GAIN_MIN) g = EO_GAIN_MIN;
    if (g > gaincap)     g = gaincap;                  /* accept dim past here */

    ae->exp_lines = (int)lround(exp);
    ae->vmax      = vmax;
    ae->gain      = g;
}
