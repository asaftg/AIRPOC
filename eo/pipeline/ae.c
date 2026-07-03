/* Flicker-free auto-exposure, "expose don't gain" — at a FIXED frame rate.
 *
 * The operating fps is set by the operator (ae->vmax) and is a hard constraint: it
 * caps the maximum exposure (max integration = vmax - SHS1_MIN) and the AE NEVER
 * changes it. No frame dropping. Within that fixed exposure budget the AE:
 *   - when off-target, corrects brightness in the log domain (damped + slew-capped);
 *     on-target, holds brightness;
 *   - realizes it EVERY tick on a two-rung ladder: exposure time first (up to the
 *     fps-capped ceiling), then analog gain (hard-capped at gaincap).
 * Because the ladder re-runs every tick, gain is continuously MINIMIZED: any gain the
 * exposure ceiling can absorb is shifted off gain at constant brightness. Gain only
 * rises once exposure is maxed for the current fps — the only way to get more light
 * without dropping frames. Want more exposure headroom? Lower the operating fps (a
 * deliberate operator choice), not something the AE does behind your back. */
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

    double B = ae->exp_lines * gain_lin(ae->gain) * factor;   /* target brightness */

    /* Rung 1: spend exposure, up to the ceiling the FIXED operating fps allows.
     * ae->vmax is set by the operator and is NOT modified here — no frame dropping. */
    double exp = B;
    double exp_ceiling = ae->vmax - EO_SHS1_MIN;       /* max integration at this fps */
    if (exp > exp_ceiling) exp = exp_ceiling;
    if (exp < EO_MIN_EXP_LINES) exp = EO_MIN_EXP_LINES;

    /* Rung 2: only the brightness exposure couldn't reach at this fps -> gain, capped. */
    double g_lin_needed = B / exp;
    if (g_lin_needed < 1.0) g_lin_needed = 1.0;
    int g = (int)lround(20.0 * log10(g_lin_needed) / 0.1);
    if (g < EO_GAIN_MIN) g = EO_GAIN_MIN;
    if (g > gaincap)     g = gaincap;                  /* accept dim past here */

    ae->exp_lines = (int)lround(exp);
    ae->gain      = g;
    /* ae->vmax intentionally left unchanged — the fps is fixed. */
}
