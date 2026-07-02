/* Flicker-free auto-exposure. Identical law to the validated bench tool:
 * filter the metric (EMA), act in the log-brightness domain with damping + a
 * per-update slew cap and a wide deadband, spend exposure first (best SNR) and
 * gain only when exposure is maxed. No fixed additive steps (those pump near a
 * bright source). */
#include "pipeline.h"
#include <math.h>

/* register step is 0.1 dB -> linear brightness factor */
static double gain_lin(int g) { return pow(10.0, g * 0.1 / 20.0); }

void ae_init(AE *ae)
{
    ae->exp_lines = 725;               /* ~10.7 ms; shs1 = VMAX - 725 = 400 */
    ae->gain      = 40;
    ae->mean_ema  = EO_AE_TARGET;
    ae->mean      = 0.0;
}

void ae_update(AE *ae, double mean10)
{
    ae->mean = mean10;
    ae->mean_ema = 0.6 * ae->mean_ema + 0.4 * mean10;

    double ratio = EO_AE_TARGET / (ae->mean_ema > 1.0 ? ae->mean_ema : 1.0);
    if (ratio > 0.90 && ratio < 1.11) return;          /* +/-10% deadband */

    double factor = sqrt(ratio);                       /* damping: half the error */
    if (factor > 1.5)   factor = 1.5;                  /* slew cap */
    if (factor < 0.667) factor = 0.667;

    double B = ae->exp_lines * gain_lin(ae->gain) * factor;   /* target brightness */

    double exp = B;                                    /* spend exposure first */
    if (exp > EO_MAX_EXP_LINES) exp = EO_MAX_EXP_LINES;
    if (exp < EO_MIN_EXP_LINES) exp = EO_MIN_EXP_LINES;

    double g_lin_needed = B / exp;                     /* remainder -> gain */
    if (g_lin_needed < 1.0) g_lin_needed = 1.0;
    int g = (int)lround(20.0 * log10(g_lin_needed) / 0.1);
    if (g < EO_GAIN_MIN) g = EO_GAIN_MIN;
    if (g > EO_GAIN_MAX) g = EO_GAIN_MAX;

    ae->exp_lines = (int)lround(exp);
    ae->gain      = g;
}
