/* tdn.h — temporal display denoiser (P0 of the night image-quality plan).
 *
 * Motion-adaptive temporal IIR on the RAW native Y10, BEFORE the tonemap — the night
 * noise (~0.7 LSB at 10-bit) lives below the 8-bit floor until the display stretch
 * amplifies it, so denoising must happen in the raw domain. DISPLAY-ONLY by design:
 * the detector keeps consuming the raw tap (red-team verdict: temporal denoise
 * attenuates exactly the faint slow movers the detector exists to catch).
 *
 * Red-team-mandated design points (see eo/docs/NIGHT_IQ.md):
 *  - 4x4 block-POOLED motion test (pools a faint mover's contrast; a per-pixel test
 *    is provably blind to a 300 m walker at 0.27 px/frame)
 *  - AE steps: accumulator SCALED by the applied exposure*gain ratio (never reset),
 *    using libeo's +2-frame register-landing model (eo_frame_ae)
 *  - noise threshold is EMPIRICAL per intensity bin (tracks read + shot noise at the
 *    applied gain; no hard-coded sensor model)
 *  - error-feedback accumulation (no Q10.5 dead-band bias)
 *  - row-offset correction against the accumulator reference; rows without enough
 *    static pixels get ZERO correction (never neighbor interpolation)
 *  - global-motion guard: >60% moving blocks = slew/unmodeled event -> pass-through
 *    (display shows raw; the accumulator reseeds). Static-mount instrument, interim
 *    by contract until a warp-accumulation design exists for the tracking gimbal.
 *  - night-gated with hysteresis; gated off = caller uses the raw frame, byte-identical
 *
 * Output: u16 little-endian per pixel, Q10.5 in bits [15:1] (bit 0 = 0). The tonemap
 * consumes the 5 fractional bits (in_q5=1) so the accumulator's sub-LSB precision
 * survives to the dithered 8-bit output instead of being truncated.
 */
#ifndef EO_TDN_H
#define EO_TDN_H

#include <stdint.h>

/* Process one native frame. y10 = packed raw (u16 LE, 10-bit in bits [15:6]).
 * Writes the denoised frame to out (u16 LE, Q10.5 in bits [15:1]) and returns 1,
 * or returns 0 when disabled / day-gated / pass-through (caller uses the raw frame).
 * exp_lines/gain = the APPLIED sensor state for this frame (eo_frame_ae). */
int  tdn_process(const uint8_t *y10, int bpl, int w, int h,
                 uint16_t *out, int exp_lines, int gain);

void tdn_set_enabled(int on);      /* operator knob (GUI /ctl?denoise=)          */
int  tdn_enabled(void);
int  tdn_active(void);             /* did the night gate actually run last frame */
double tdn_last_ms(void);          /* measured cost of the last processed frame  */

#endif /* EO_TDN_H */
