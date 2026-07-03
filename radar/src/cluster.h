/* DBSCAN + constant-velocity Kalman tracker for the AWR2944P point cloud.
 *
 * Port of the ground bench's radar/clustering.py — the PRIMARY box source
 * while the firmware has no on-chip Group Tracker (see docs/FIRMWARE.md).
 * Per frame: DBSCAN over (x,y,z,doppler) -> per-cluster centroid/extent ->
 * greedy gated NN association (stable ids) -> CV Kalman update (velocity) ->
 * M-of-N confirm. Emits ONLY targets detected this frame — no coasting.
 * Motion-model coasting/persistence is the future tracking module + GUI.
 *
 * The 6-state [p,v] Kalman of the original is block-diagonal per axis, so
 * this is implemented as three independent 2-state (pos,vel) filters —
 * identical maths, much simpler in C. This module is deliberately behind
 * a clean interface so on-chip gtrack TLVs (308/309) can replace it later
 * without touching the parser or the previewer. */
#ifndef AIRPOC_CLUSTER_H
#define AIRPOC_CLUSTER_H

#include "radar.h"

typedef struct RadarClusterer RadarClusterer;

/* Live-tunable DBSCAN knobs — defaults + clamp bounds, settable via the
 * daemon's /ctl endpoint (CLUSTER eps + MIN PTS sliders in the GUI). */
#define CLUSTER_DEFAULT_EPS_M    8.0
#define CLUSTER_DEFAULT_MIN_PTS  2
#define CLUSTER_DEFAULT_SPEED    0.4    /* m/s  — dynamic-only gate */
#define CLUSTER_DEFAULT_SNR      0.0    /* dB   — 0 = no SNR gate (publish-max) */
#define CLUSTER_DEFAULT_FOV      90.0   /* deg  — azimuth half-angle gate (90 = full) */
#define CLUSTER_DEFAULT_DOP      3.0    /* m/s  — doppler-similarity gate */
#define CLUSTER_EPS_MIN_M        0.5
#define CLUSTER_EPS_MAX_M        50.0
#define CLUSTER_MIN_PTS_MIN      1
#define CLUSTER_MIN_PTS_MAX      20
#define CLUSTER_SPEED_MIN        0.0
#define CLUSTER_SPEED_MAX        5.0
#define CLUSTER_SNR_MIN          0.0
#define CLUSTER_SNR_MAX          60.0
#define CLUSTER_FOV_MIN          5.0
#define CLUSTER_FOV_MAX          90.0
#define CLUSTER_DOP_MIN          0.5
#define CLUSTER_DOP_MAX          20.0

RadarClusterer *cluster_new(void);
void            cluster_free(RadarClusterer *c);

/* Set DBSCAN spacing (eps, metres) and min-samples live. Clamped to sane
 * ranges. A single scalar store per field — safe to call from the HTTP
 * thread while the radar thread clusters (worst case: one frame uses a
 * just-changed value). */
void cluster_set_dbscan(RadarClusterer *c, double eps_m, int min_pts);

/* Set the four live gates: min |radial speed| (m/s, dynamic-only), min
 * per-point SNR (dB; 0=off), azimuth half-angle (deg; |az|>fov excluded),
 * and the doppler-similarity gate (m/s). Clamped. Same thread-safety note. */
void cluster_set_gates(RadarClusterer *c, double speed_min_mps, double snr_min_db,
                       double fov_half_deg, double doppler_gate_mps);

/* Current azimuth-gate half-angle (deg) — published on the wire for the wedge. */
double cluster_fov(const RadarClusterer *c);

/* Cluster+track one frame. `pts`/`n` are updated in place (each point's
 * .tid is set to its cluster/track id, or 255). `now_s` is a monotonic
 * timestamp (seconds); `dt` is seconds since the previous step. Confirmed
 * targets detected THIS frame are written to `out` up to `max_out`; returns
 * the count. */
int cluster_step(RadarClusterer *c, RadarPoint *pts, int n,
                 double now_s, double dt,
                 RadarTarget *out, int max_out);

#endif /* AIRPOC_CLUSTER_H */
