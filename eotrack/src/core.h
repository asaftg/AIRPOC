/* core.h - the EO tracker core: association, filtering, lifecycle, and the
 * translate-vs-oscillate clutter latch. Pure, allocation-free after trk_core_new,
 * and independent of HTTP/SSE/taps so it can be driven directly from a replay
 * harness (tools/replay.c) for deterministic offline validation.
 *
 * Everything internal is in PIXELS (that is what the detector reports and what the
 * lock loop needs). Angles are derived at emit time (emit.c) from the IFOV.
 */
#ifndef TRK_CORE_H
#define TRK_CORE_H

#include <stdint.h>
#include <stddef.h>

/* One detection parsed from the detector wire, in the EO pixel frame. */
typedef struct {
    int    src;      /* 0 = appearance model ("app"), 1 = motion worker ("mot") */
    int    cls;      /* 0 = unknown/mover, 1 = human, 2 = vehicle, 3 = drone */
    double conf;
    double cx, cy, w, h;   /* box centre + size, px */
    int    tbd;      /* 1 = promoted by the detector's temporal integration */
    int    hits;     /* detector-side evidence count (-1 if absent) */
    int    age;      /* detector-side track age in ticks (-1 if absent) */
} TrkDet;

/* One track as emitted on the wire (px; emit.c converts to angles). */
typedef struct {
    int         tid;
    const char *state;     /* "tent" | "conf" | "coast" */
    int         cls;       /* majority-vote class code (0..3) */
    double      cls_conf;
    double      conf;      /* smoothed detection confidence */
    double      cx, cy, w, h;    /* smoothed box, px */
    double      vx, vy;          /* px/s (position-derived) */
    double      s_px;            /* position sigma, px (fusion association gate) */
    double      grow;            /* relative size growth, 1/s (looming) */
    int         hits;            /* lifetime hit count (np analog) */
    double      age_s;
    double      coast_s;         /* seconds since last real measurement */
    uint64_t    t_meas_ns;       /* t_src of the last measurement (correlation key) */
    int         src;             /* 0 app, 1 mot, 2 both (dominant evidence source) */
    int         tbd;             /* 1 = ever promoted from faint evidence (detector tbd) */
    int         lock_on;         /* engaged track only */
    double      lock_score;
} TrkOut;

typedef struct {
    double gate_base;    /* px */
    double confirm;      /* score to confirm */
    double coast_s;
    double clutter_s;
} TrkKnobs;

typedef struct TrkCore TrkCore;

TrkCore *trk_core_new(void);
void     trk_core_free(TrkCore *c);
void     trk_core_set_knobs(TrkCore *c, const TrkKnobs *k);

/* Advance the tracker by one detector tick.
 *   dets/n        parsed detections this tick
 *   t_src_ns      the EO frame's source timestamp (correlation key; NOT wall-clock)
 *   dt_s          measured seconds since the previous tick (>0; drives rate scaling)
 *   ego_dx/ego_dy global scene shift since last tick, px (0 on a static mount) -
 *                 subtracted before the clutter net-displacement test so a panning
 *                 camera does not read as everything translating
 *   engaged_tid   operator-selected track id, or -1
 *   out/max       emitted (latch-passing) tracks written here
 * Returns the number of tracks written to out.
 */
int trk_core_step(TrkCore *c, const TrkDet *dets, int n,
                  uint64_t t_src_ns, double dt_s,
                  double ego_dx, double ego_dy, int engaged_tid,
                  TrkOut *out, int max);

/* Emit the current confirmed + latch-passing tracks WITHOUT advancing time. Lets the
 * 60 fps lock loop republish the wire between detector ticks after nudging the engaged
 * track. Returns the number written to out. */
int trk_core_snapshot(TrkCore *c, int engaged_tid, TrkOut *out, int max);

/* Diagnostics for /stats: total live tracks (incl. latched-off) and emitted last tick. */
void trk_core_counts(const TrkCore *c, int *live, int *emitted);

/* 1 if a live track with this id exists (an engaged track is kept alive by the sticky
 * coast, so this only returns 0 for an engaged id that never existed or died before the
 * engage arrived - the daemon then releases the lock). */
int trk_core_has_track(const TrkCore *c, int tid);

/* The engaged track's current predicted centre/size in px, for the lock loop to
 * seed/anchor its ROI. Returns 1 if the engaged track exists, else 0. */
int trk_core_engaged_box(const TrkCore *c, int engaged_tid,
                         double *cx, double *cy, double *w, double *h, int *cls);

/* Feed the lock loop's sub-pixel result back as the engaged track's measurement
 * for a frame with no detection (keeps the wire at camera rate between NN ticks). */
void trk_core_lock_update(TrkCore *c, int engaged_tid,
                          double cx, double cy, double score, uint64_t t_src_ns);

#endif /* TRK_CORE_H */
