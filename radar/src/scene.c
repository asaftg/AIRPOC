/* scene.c — static occupancy layer. See scene.h. */
#include "scene.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define NCELL (SCENE_NR * SCENE_NA)

struct SceneMap {
    float         hits[NCELL];    /* discounted count of frames the cell was lit */
    unsigned char peak[NCELL];    /* strongest echo seen in the cell, dB         */
    unsigned char seen[NCELL];    /* per-frame scratch                           */
    float         frames_eff;     /* discounted frame count (the denominator)    */
    unsigned int  frames_raw;     /* frames since reset, for "map age"           */
    double        halflife_s;     /* 0 = never forget                            */
    int           enabled;
};

SceneMap *scene_new(void) {
    SceneMap *s = (SceneMap *)calloc(1, sizeof(*s));
    if (s) { s->enabled = 1; s->halflife_s = SCENE_HALFLIFE_DEFAULT; }
    return s;
}
void scene_free(SceneMap *s) { free(s); }

void scene_reset(SceneMap *s) {
    if (!s) return;
    memset(s->hits, 0, sizeof(s->hits));
    memset(s->peak, 0, sizeof(s->peak));
    s->frames_eff = 0.0f; s->frames_raw = 0;
}
void scene_set_enabled(SceneMap *s, int on) { if (s) s->enabled = on ? 1 : 0; }
int  scene_enabled(const SceneMap *s) { return s ? s->enabled : 0; }
void scene_set_halflife(SceneMap *s, double seconds) {
    if (!s) return;
    if (seconds < 0.0) seconds = 0.0;
    s->halflife_s = seconds;
}
double scene_halflife(const SceneMap *s) { return s ? s->halflife_s : 0.0; }

void scene_step(SceneMap *s, const RadarPoint *pts, int n, double dt) {
    if (!s || !s->enabled) return;

    /* Exponential forgetting. Old evidence fades smoothly instead of falling off
     * the edge of a window, and it costs one multiply per cell per frame.
     * Discounting the denominator too keeps occupancy = hits/frames_eff a true
     * fraction, and makes frames_eff saturate at halflife/ln2 * rate. */
    if (s->halflife_s > 0.0 && dt > 0.0) {
        float lam = (float)exp(-M_LN2 * dt / s->halflife_s);
        if (lam < 0.0f) lam = 0.0f;
        if (lam < 1.0f) {
            for (int c = 0; c < NCELL; c++) {
                s->hits[c] *= lam;
                if (s->hits[c] < 1e-3f) { s->hits[c] = 0.0f; s->peak[c] = 0; }
            }
            s->frames_eff *= lam;
        }
    }

    s->frames_eff += 1.0f;
    s->frames_raw++;
    memset(s->seen, 0, sizeof(s->seen));

    for (int i = 0; i < n; i++) {
        const RadarPoint *p = &pts[i];
        if (fabsf(p->doppler) >= SCENE_DOP_MAX) continue;       /* it is moving */
        if (!(p->range > 8.0f) || p->range >= SCENE_NR * SCENE_RSTEP) continue;
        float azrel = p->az - SCENE_AZ0;
        if (azrel < 0.0f || azrel >= SCENE_NA * SCENE_ASTEP) continue;
        int ir = (int)(p->range / SCENE_RSTEP);
        int ia = (int)(azrel / SCENE_ASTEP);
        if (ir < 0 || ir >= SCENE_NR || ia < 0 || ia >= SCENE_NA) continue;
        int c = ir * SCENE_NA + ia;
        if (!s->seen[c]) { s->seen[c] = 1; s->hits[c] += 1.0f; }  /* one vote per frame */
        if (isfinite(p->snr) && p->snr > 0.0f) {
            int db = (int)(p->snr + 0.5f);
            if (db > 255) db = 255;
            if ((unsigned char)db > s->peak[c]) s->peak[c] = (unsigned char)db;
        }
    }
}

/* {"scene":1,"frames":N,"halflife_s":..,"nr":..,"na":..,"r_step":..,"az0":..,
 *  "az_step":..,"cells":[ri,ai,occ,snr, ...]}   occ = 0..255 */
int scene_json(const SceneMap *s, char *buf, size_t cap) {
    if (!s || cap < 256) return 0;
    size_t off = 0;
    int w = snprintf(buf, cap,
        "{\"scene\":%d,\"frames\":%u,\"halflife_s\":%.1f,\"nr\":%d,\"na\":%d,"
        "\"r_step\":%.3f,\"az0\":%.1f,\"az_step\":%.2f,\"cells\":[",
        s->enabled, s->frames_raw, s->halflife_s, SCENE_NR, SCENE_NA,
        (double)SCENE_RSTEP, (double)SCENE_AZ0, (double)SCENE_ASTEP);
    if (w < 0 || (size_t)w >= cap) return 0;
    off = (size_t)w;

    float fr = s->frames_eff > 1e-6f ? s->frames_eff : 1.0f;
    int first = 1;
    for (int c = 0; c < NCELL; c++) {
        if (s->hits[c] <= 0.0f) continue;
        int occ = (int)((s->hits[c] / fr) * 255.0f + 0.5f);
        if (occ > 255) occ = 255;
        if (occ <= 0) continue;
        int ir = c / SCENE_NA, ia = c % SCENE_NA;
        if (off + 32 >= cap) break;
        w = snprintf(buf + off, cap - off, "%s%d,%d,%d,%u",
                     first ? "" : ",", ir, ia, occ, (unsigned)s->peak[c]);
        if (w < 0 || (size_t)w >= cap - off) break;
        off += (size_t)w; first = 0;
    }
    if (off + 3 >= cap) return 0;
    buf[off++] = ']'; buf[off++] = '}'; buf[off] = '\0';
    return (int)off;
}
