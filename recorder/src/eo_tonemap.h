/* eo_tonemap — Y10 -> 8-bit human-view tone map, for native-resolution replay.
 *
 * The RECORDER's own copy of the EO pipeline's tone map, kept byte-for-byte
 * identical to eo/pipeline/isp.c:isp_scale_tonemap so native replay shows
 * exactly what the operator could have seen — at full detail. It lives here,
 * not in eo/, so the recorder never reaches into the EO module.
 *
 * Being a copy, it could drift from the EO feed — but never silently: the
 * recorder stamps a hash of this tone map's output (eo_tonemap_hash) into every
 * recording, and replay recomputes it and reports `tonemap_match:false` if the
 * running build differs from the tone map that recorded the session. If the EO
 * feed's tone map ever changes, mirror it here and bump EO_TONEMAP_VERSION.
 *
 * Algorithm source of truth: eo/pipeline/isp.c + pipeline.h (EO_GAMMA=0.85,
 * EO_MIN_SPAN=40) as of the version below.
 */
#ifndef EO_TONEMAP_H
#define EO_TONEMAP_H

#include <stdint.h>

#define EO_TONEMAP_VERSION 1

#define EO_TONE_GAMMA     0.85          /* display gamma (encode: v^(1/gamma))   */
#define EO_TONE_MIN_SPAN  40.0          /* min p1..p99 span (10-bit counts): a    */
                                        /* flat/dim scene is NOT blown to full    */
                                        /* range (would be ~6x noise gain)        */

/* Temporal EMA of the black/white endpoints — carried by the caller so the
 * mapping doesn't wobble frame-to-frame. Zero it to seed on the next call. */
typedef struct { double s_lo, s_hi; int seeded; } EoToneState;

/* Crop (cx,cy,cw,ch) of the Y10 frame (left-justified 16-bit LE, stride bpl),
 * box-average downscale to ow*oh, adaptive p1/p99 white/black stretch (EMA via
 * st, span floored) + gamma -> 8-bit out8.
 *   st : caller-owned EMA state (persist across frames for anti-breathing)
 *   sm : caller scratch, >= ow*oh uint16   (no per-call malloc on the hot path)
 *   xs : caller scratch, >= (ow+1) int
 *   mx,my,mw,mh : metering sub-rect in OUTPUT (sm) coords — the p1/p99 stretch is
 *     computed over only this rect, not the whole output. Pass mw<=0 (or mh<=0)
 *     to meter the whole output (the byte-identical original behaviour, and what
 *     eo_tonemap_hash uses). Replay uses this so a native full-frame render can be
 *     exposed for the region the operator was actually viewing (their zoom crop),
 *     instead of the whole mostly-dark frame over-stretching and blowing out.
 * For full-native output pass cx=cy=0, cw=ow=w, ch=oh=h. */
void eo_tonemap(const uint8_t *y10, int bpl, int cx, int cy, int cw, int ch,
                uint8_t *out8, int ow, int oh, EoToneState *st,
                uint16_t *sm, int *xs, int mx, int my, int mw, int mh);

/* 3x3 median grain filter (edge-preserving), in place on an 8-bit image.
 * scratch: caller-owned, >= w*h bytes. */
void eo_median3(uint8_t *img, int w, int h, uint8_t *scratch);

/* Drift signature. Runs the tone map on a fixed canonical frame and returns a
 * hash of the 8-bit output — ANY change to the tone-map math changes it. The
 * recorder stamps this (device-computed) into each recording's eo_y10
 * channel.json; replay recomputes it and flags `tonemap_match:false` if they
 * differ, so a session recorded under different tone-map math than the current
 * build is caught rather than silently re-rendered wrong. (Computed on the same
 * device at record and replay time, so libm differences across CPUs never cause
 * a false mismatch.) */
uint32_t eo_tonemap_hash(void);

#endif
