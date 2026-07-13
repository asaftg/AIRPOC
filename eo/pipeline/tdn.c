/* tdn.c — see tdn.h. Two passes per frame over 4-pixel-high strips:
 *   pass A (per strip): per-row static-referenced offset, then 4x4 block sums of the
 *            offset-corrected frame and of the accumulator
 *   between: per-block motion score vs the empirical per-intensity-bin noise scale,
 *            3x3 block dilation, night-gate / global-motion decisions
 *   pass B (per strip): per-pixel IIR update with the block alpha + error feedback,
 *            emit Q10.5
 * DRAM traffic ~9 MB/frame (~0.6 GB/s at 60 fps) — small next to the 102 GB/s bus;
 * the budget target is <1 core at pinned clocks (measure with tools/tdn_bench). */
#include "tdn.h"
#include "pipeline.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- tuning (eo/docs/NIGHT_IQ.md derives each) ---- */
#define TDN_BLK          4                  /* block edge (px)                       */
#define TDN_ALPHA_MIN_Q8 16                 /* 1/16 — static-region IIR gain         */
#define TDN_K1_Q4        32                 /* ramp start: 2.0 * sigma_block         */
#define TDN_K2_Q4        64                 /* ramp end (alpha=1): 4.0 * sigma_block */
#define TDN_ROWOFF_MAX   (3 << 5)           /* row correction clamp: 3 LSB, Q10.5    */
#define TDN_ROW_MIN_PX   180                /* min static px/row for a row offset    */
#define TDN_GATE_GAIN_ON  200               /* engage above 20 dB applied gain ...   */
#define TDN_GATE_GAIN_OFF 160               /* ... disengage below 16 dB (hysteresis)*/
#define TDN_GATE_FRAMES   8                 /* consecutive frames to flip the gate   */
#define TDN_GLOBAL_FRAC   60                /* % moving blocks = global event        */
#define TDN_NBINS        8                  /* intensity bins for the noise scale    */
#define TDN_SETTLE       2                  /* frames to freeze stats after AE scale */

static volatile int g_on = 1;               /* operator knob (default on; gated)     */
static volatile int g_active = 0;
static volatile double g_ms = 0.0;

static struct {
    int      w, h, bw, bh;
    uint16_t *acc;         /* Q10.5 accumulator                                   */
    uint8_t  *err;         /* error-feedback residue (per pixel)                  */
    uint8_t  *alpha;       /* per-block alpha this frame (Q8: 16..255)            */
    uint8_t  *moving;      /* per-block moving flag, previous frame (row-offset mask) */
    uint8_t  *score;       /* per-block raw motion score before dilation          */
    uint32_t *sy, *sa;     /* per-block-column strip sums (y', acc)               */
    int16_t  *rowoff;      /* per-row offset, Q10.5                               */
    /* empirical noise scale per intensity bin: EMA of mean |block diff| (Q10.5)  */
    uint32_t  nscale[TDN_NBINS];
    int       seeded, gate, gate_run, settle;
    double    prod;        /* applied exposure*gain product of the accumulator    */
} T;

static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void tdn_set_enabled(int on) { g_on = on ? 1 : 0; }
int  tdn_enabled(void)       { return g_on; }
int  tdn_active(void)        { return g_active; }
double tdn_last_ms(void)     { return g_ms; }

static int tdn_alloc(int w, int h)
{
    T.w = w; T.h = h; T.bw = w / TDN_BLK; T.bh = h / TDN_BLK;
    T.acc    = malloc((size_t)w * h * 2);
    T.err    = malloc((size_t)w * h);
    T.alpha  = malloc((size_t)T.bw * T.bh);
    T.moving = calloc((size_t)T.bw * T.bh, 1);
    T.score  = malloc((size_t)T.bw * T.bh);
    T.sy     = malloc((size_t)T.bw * 4);
    T.sa     = malloc((size_t)T.bw * 4);
    T.rowoff = malloc((size_t)h * 2);
    for (int i = 0; i < TDN_NBINS; i++) T.nscale[i] = 2 << 5;  /* seed ~2 LSB */
    T.seeded = 0; T.gate = 0; T.gate_run = 0; T.settle = 0; T.prod = 0.0;
    return T.acc && T.err && T.alpha && T.moving && T.score && T.sy && T.sa && T.rowoff;
}

/* seed the accumulator from the current frame (gate engage / first frame) */
static void tdn_seed(const uint8_t *y10, int bpl)
{
    for (int y = 0; y < T.h; y++) {
        const uint8_t *row = y10 + (size_t)y * bpl;
        uint16_t *arow = T.acc + (size_t)y * T.w;
        for (int x = 0; x < T.w; x++)
            arow[x] = (uint16_t)(((row[2*x] | (row[2*x+1] << 8)) >> 6) << 5);
    }
    memset(T.err, 0, (size_t)T.w * T.h);
    memset(T.moving, 0, (size_t)T.bw * T.bh);
    T.seeded = 1;
}

int tdn_process(const uint8_t *y10, int bpl, int w, int h,
                uint16_t *out, int exp_lines, int gain)
{
    if (!g_on || w < 64 || h < 64) { g_active = 0; return 0; }
    if (T.w != w || T.h != h) {
        free(T.acc); free(T.err); free(T.alpha); free(T.moving);
        free(T.score); free(T.sy); free(T.sa); free(T.rowoff);
        if (!tdn_alloc(w, h)) { g_active = 0; return 0; }
    }
    double t0 = now_ms();

    /* ---- night gate FIRST (before any per-pixel work): keys on APPLIED gain with
     * hysteresis. High analog gain only happens in the dark (the AE minimizes gain;
     * a bright scene runs at ~0), so gain IS the "dark + stretched" correlate.
     * Gated off => zero cost, display byte-identical to the raw path. */
    int want = gain >= TDN_GATE_GAIN_ON ? 1 : (gain <= TDN_GATE_GAIN_OFF ? 0 : T.gate);
    if (want != T.gate) {
        if (++T.gate_run >= TDN_GATE_FRAMES) { T.gate = want; T.gate_run = 0; }
    } else T.gate_run = 0;
    if (!T.gate) { g_active = 0; T.seeded = 0; return 0; }

    /* ---- AE handling: scale the accumulator by the APPLIED brightness ratio.
     * Never reset — a reset throws away the integrated SNR and pulses noise. */
    double prod = (double)exp_lines * pow(10.0, gain * 0.1 / 20.0);
    if (T.seeded && T.prod > 0.0) {
        double ratio = prod / T.prod;
        if (ratio < 0.99 || ratio > 1.01) {
            int32_t rq = (int32_t)lround(ratio * 4096.0);
            size_t n = (size_t)w * h;
            for (size_t i = 0; i < n; i++) {
                uint32_t v = ((uint32_t)T.acc[i] * (uint32_t)rq) >> 12;
                T.acc[i] = (uint16_t)(v > 32736 ? 32736 : v);
            }
            T.settle = TDN_SETTLE;         /* freeze stats while the step lands */
        }
    }
    T.prod = prod;

    if (!T.seeded) tdn_seed(y10, bpl);

    /* ---- pass A: per-strip row offsets + block sums ---- */
    int bw = T.bw, bh = T.bh;
    for (int by = 0; by < bh; by++) {
        memset(T.sy, 0, (size_t)bw * 4);
        memset(T.sa, 0, (size_t)bw * 4);
        for (int sy = 0; sy < TDN_BLK; sy++) {
            int y = by * TDN_BLK + sy;
            const uint8_t *row = y10 + (size_t)y * bpl;
            const uint16_t *arow = T.acc + (size_t)y * w;
            const uint8_t *mrow = T.moving + (size_t)by * bw;
            /* row offset: mean (y - acc) over pixels in blocks static LAST frame,
             * BEFORE the motion test (temporal row noise must not trip whole rows) */
            int64_t sum = 0; int cnt = 0;
            for (int bx = 0; bx < bw; bx++) {
                if (mrow[bx]) continue;
                int x0 = bx * TDN_BLK;
                for (int i = 0; i < TDN_BLK; i++) {
                    int x = x0 + i;
                    int yv = ((row[2*x] | (row[2*x+1] << 8)) >> 6) << 5;
                    sum += yv - arow[x]; cnt++;
                }
            }
            int off = 0;
            if (cnt >= TDN_ROW_MIN_PX && T.settle == 0) {
                off = (int)(sum / cnt);
                if (off >  TDN_ROWOFF_MAX) off =  TDN_ROWOFF_MAX;
                if (off < -TDN_ROWOFF_MAX) off = -TDN_ROWOFF_MAX;
            }                               /* too few static px -> ZERO, never
                                             * a neighbor's offset (uncorrelated) */
            T.rowoff[y] = (int16_t)off;
            /* block sums of the corrected frame + accumulator */
            for (int bx = 0; bx < bw; bx++) {
                int x0 = bx * TDN_BLK; uint32_t s1 = 0, s2 = 0;
                for (int i = 0; i < TDN_BLK; i++) {
                    int x = x0 + i;
                    s1 += (uint32_t)((((row[2*x] | (row[2*x+1] << 8)) >> 6) << 5) - off);
                    s2 += arow[x];
                }
                T.sy[bx] += s1; T.sa[bx] += s2;
            }
        }
        /* per-block motion score: |mean(y') - mean(acc)| in Q10.5 (pooled over 16 px
         * — this is the red-team fix: pooling lets a faint coherent mover trip the
         * test that a per-pixel diff provably cannot) */
        uint8_t *srow = T.score + (size_t)by * bw;
        for (int bx = 0; bx < bw; bx++) {
            int32_t my = (int32_t)(T.sy[bx] >> 4), ma = (int32_t)(T.sa[bx] >> 4);
            int32_t d = my > ma ? my - ma : ma - my;
            int bin = (my >> 5) >> 7; if (bin >= TDN_NBINS) bin = TDN_NBINS - 1;
            uint32_t ns = T.nscale[bin] ? T.nscale[bin] : 1;
            /* score in 1/16ths of the noise scale, saturated */
            uint32_t sc = (uint32_t)d * 16 / ns;
            srow[bx] = sc > 255 ? 255 : (uint8_t)sc;
            /* empirical noise EMA from sub-threshold blocks (skip during settle) */
            if (sc < TDN_K1_Q4 && T.settle == 0)
                T.nscale[bin] += ((uint32_t)d * 5 / 4 - T.nscale[bin]) / 64;
        }
    }
    if (T.settle > 0) T.settle--;

    /* ---- gate + global-motion decisions on the block stats ---- */
    int moving_blocks = 0;
    for (int i = 0; i < bw * bh; i++) if (T.score[i] >= TDN_K1_Q4) moving_blocks++;
    int global = moving_blocks * 100 > bw * bh * TDN_GLOBAL_FRAC;

    if (global) {
        /* slew / unmodeled global event: show raw, reseed the reference */
        tdn_seed(y10, bpl);
        for (int y = 0; y < h; y++) {
            const uint8_t *row = y10 + (size_t)y * bpl;
            uint16_t *orow = out + (size_t)y * w;
            for (int x = 0; x < w; x++)
                orow[x] = (uint16_t)((((row[2*x] | (row[2*x+1] << 8)) >> 6) << 5) << 1);
        }
        g_active = 1; g_ms = now_ms() - t0;
        return 1;
    }

    /* ---- per-block alpha with 3x3 dilation (max of neighbors) ---- */
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint8_t mx = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = by + dy; if (yy < 0 || yy >= bh) continue;
                const uint8_t *sr = T.score + (size_t)yy * bw;
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = bx + dx; if (xx < 0 || xx >= bw) continue;
                    if (sr[xx] > mx) mx = sr[xx];
                }
            }
            int a;
            if (mx <= TDN_K1_Q4)      a = TDN_ALPHA_MIN_Q8;
            else if (mx >= TDN_K2_Q4) a = 255;
            else a = TDN_ALPHA_MIN_Q8 +
                     (255 - TDN_ALPHA_MIN_Q8) * (mx - TDN_K1_Q4) / (TDN_K2_Q4 - TDN_K1_Q4);
            T.alpha[(size_t)by * bw + bx]  = (uint8_t)a;
            T.moving[(size_t)by * bw + bx] = mx > TDN_K1_Q4;
        }
    }

    /* ---- pass B: IIR update with error feedback, emit Q10.5<<1 ---- */
    for (int y = 0; y < h; y++) {
        const uint8_t *row = y10 + (size_t)y * bpl;
        uint16_t *arow = T.acc + (size_t)y * w;
        uint8_t  *erow = T.err + (size_t)y * w;
        uint16_t *orow = out + (size_t)y * w;
        const uint8_t *alr = T.alpha + (size_t)(y / TDN_BLK) * bw;
        int off = T.rowoff[y];
        for (int x = 0; x < w; x++) {
            int a = alr[x / TDN_BLK];
            int32_t yv = ((((row[2*x] | (row[2*x+1] << 8)) >> 6) << 5) - off);
            if (yv < 0) yv = 0;
            int32_t d = yv - (int32_t)arow[x];
            /* error feedback: carry the truncated remainder so sub-LSB residuals
             * converge instead of sticking (Q10.5 dead-band -> banding after 6x) */
            int32_t t = a * d + (d >= 0 ? (int32_t)erow[x] : -(int32_t)erow[x]);
            int32_t inc = t >> 8;
            erow[x] = (uint8_t)((t >= 0 ? t : -t) & 255);
            int32_t v = (int32_t)arow[x] + inc;
            if (v < 0) v = 0;
            if (v > 32736) v = 32736;
            arow[x] = (uint16_t)v;
            orow[x] = (uint16_t)(v << 1);
        }
    }

    g_active = 1;
    g_ms = now_ms() - t0;
    return 1;
}
