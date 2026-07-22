/* core.h - the pure fusion core: pairs radar-tracker targets with EO-tracker
 * tracks, keeps pairs sticky, assigns the global id, and composes the fused
 * output rows. Allocation-free after fus_core_new(); no I/O, no clock, no
 * threads - every call passes the tick time in, so the offline harnesses can
 * drive it deterministically.
 */
#ifndef FUS_CORE_H
#define FUS_CORE_H

#include <stdint.h>
#include <stddef.h>

/* ---- inputs (filled by the feed parsers) ---- */

typedef struct {            /* one radar target, from :8092 targets[] */
    int    tid;             /* <1000 cluster, >=1000 slowdet */
    double x, y, z;         /* m, sensor frame: +x right, +y forward, +z up */
    double vx, vy, vz;      /* m/s (vz is ~always 0) */
    double sx, sy, sz;      /* box half-extents, m */
    double conf;            /* 0..1 */
    int    np;              /* accumulated point count */
    int    sus;             /* 1 = suspected sidelobe copy */
    int    mv;              /* mv_class: 0 unverified-slow / 1 verified mover / 2 suspect */
} FusRadTgt;

typedef struct {            /* one EO track, from :8095 tracks[] */
    int    tid;
    int    state;           /* 0 tent / 1 conf / 2 coast */
    int    cls;             /* 0 unknown / 1 human / 2 vehicle / 3 drone */
    double cls_conf, conf;
    double az, el, aw, ah;  /* rad, raw EO sensor frame (az +right, el +up) */
    double vaz, vel;        /* rad/s */
    double s_az, s_el;      /* 1-sigma position uncertainty, rad */
    double grow;            /* relative angular-size growth, 1/s */
    double coast_s, age_s;
    int    hits;
    int    lock_on;
    double lock_score;
} FusEoTrk;

/* ---- knobs (mirrors /ctl; trim in radians here) ---- */
typedef struct {
    double trim_az, trim_el;   /* rad, ADDED to radar angles */
    double gate;               /* gate scale, 1.0 = nominal */
    int    confirm;            /* promote at confirm+1 hits of 2*confirm window */
    double divorce_s;
    double coast_s;            /* lost-constituent contribution window */
} FusKnobs;

/* ---- output row ---- */
typedef enum { FUS_SRC_RAD = 0, FUS_SRC_EO = 1, FUS_SRC_FUSED = 2 } FusSrc;

typedef struct {
    uint32_t gid;
    FusSrc   src;
    int      eo_tid, rad_tid;      /* -1 = no constituent on that side */
    double   az, el, aw, ah;       /* rad, rig frame (radar-sourced angles trimmed) */
    int      ang_src;              /* 0 = radar-sourced (coarse el), 1 = EO-sourced */
    double   vaz, vel;             /* rad/s */
    double   r_m, rdot_mps;        /* radar range/state; r_m = -1 when absent.
                                    * rdot = dr/dt: NEGATIVE = closing. */
    int      r_stale;              /* 1 = range propagated, radar side lost */
    int      cls;                  /* fusion-owned label (sticky vote) */
    double   cls_conf, conf;
    double   fused_age_s;          /* seconds since the pair confirmed (0 unfused) */
    double   eo_coast_s, rad_coast_s;  /* per-side staleness, -1 when side absent */
    double   grow;
    int      eo_hits, rad_np;      /* per-sensor evidence, -1 when absent */
    int      sus, mv;              /* radar quality flags, -1/-1 when absent */
    int      lock_on;
    double   lock_score;
} FusOut;

typedef struct FusCore FusCore;

FusCore *fus_core_new(void);
void     fus_core_free(FusCore *c);
void     fus_core_set_knobs(FusCore *c, const FusKnobs *k);
void     fus_core_get_knobs(const FusCore *c, FusKnobs *k);

/* EO geometry (from the EO wire header; fallback defaults until first frame). */
void fus_core_set_eo_geom(FusCore *c, int img_w, int img_h, double ifov_rad);

/* Feed one sensor frame; runs a full pass (associate + compose) and fills out[].
 * t_ns is the frame's CLOCK_MONOTONIC time (or sim time in the harnesses).
 * Returns the number of rows written. */
int fus_core_step_rad(FusCore *c, const FusRadTgt *t, int n, uint64_t t_ns,
                      FusOut *out, int max);
int fus_core_step_eo(FusCore *c, const FusEoTrk *t, int n, uint64_t t_ns,
                     FusOut *out, int max);

/* Heartbeat when both feeds are down: age everything, fill whatever remains. */
int fus_core_tick(FusCore *c, uint64_t t_ns, FusOut *out, int max);

/* A sensor daemon restarted (SSE drop / frame_id regression): unbind that
 * side everywhere, open the re-bind grace. sensor: 0 = radar, 1 = EO. */
void fus_core_sensor_reset(FusCore *c, int sensor, uint64_t t_ns);

void fus_core_counts(const FusCore *c, int *fused, int *eo_only, int *rad_only);

/* Observe-only mount-trim estimator (median residual over healthy fused pairs).
 * Returns sample count; est_* in radians (valid when count > 0). */
int fus_core_trim_est(const FusCore *c, double *est_az, double *est_el);

#endif
