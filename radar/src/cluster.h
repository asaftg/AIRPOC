/* Temporal multi-target tracker for the AWR2944P point cloud.
 *
 * Replaces the earlier per-frame DBSCAN+Kalman (ground-bench clustering.py port).
 * Class-agnostic track-before-detect: velocity from position history (range-rate
 * / angle-rate, never ambiguous Doppler); nearest-track association (one physical
 * target -> one track, no fragmentation); a moving channel plus a fresh-static
 * occupancy channel so a car that drives in and parks keeps its box; fast M-of-N
 * confirm; short coast; spatial dedup so co-located tracks emit one box.
 * Validated offline against the ground-truth recording (host tool: radar/tools).
 *
 * Behind the SAME interface as before, so on-chip gtrack TLVs (308/309) can
 * replace it later without touching the parser, wire, or previewer.
 *
 * The GUI /ctl knobs map onto the tracker as:
 *   CLUSTER eps  -> dedup radius (co-located tracks -> one box)
 *   MIN PTS      -> points needed to seed a track
 *   MIN SPD      -> Doppler motion threshold (moving channel)
 *   MIN SNR      -> point strength gate (static channel = +3 dB)
 *   FOV          -> azimuth gate (input + emit)
 *   DOPPLER      -> velocity-coherence gate for the duplicate-merge
 */
#ifndef AIRPOC_CLUSTER_H
#define AIRPOC_CLUSTER_H

#include "radar.h"

typedef struct RadarClusterer RadarClusterer;

/* Live-tunable knobs — defaults (GUI slider start positions) + clamp bounds,
 * settable via the daemon's /ctl endpoint. Defaults are the offline-validated
 * tracker operating point. */
#define CLUSTER_DEFAULT_EPS_M    4.5    /* m   — dedup radius (was DBSCAN eps) */
#define CLUSTER_DEFAULT_MIN_PTS  2      /*     — points to seed a track */
#define CLUSTER_DEFAULT_SPEED    0.7    /* m/s — Doppler motion threshold */
#define CLUSTER_DEFAULT_SNR      16.0   /* dB  — point strength gate */
#define CLUSTER_DEFAULT_FOV      90.0   /* deg — azimuth half-angle gate (90 = full) */
#define CLUSTER_DEFAULT_DOP      1.2    /* m/s — merge velocity-coherence gate */
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

/* eps -> dedup radius (m); min_pts -> seed points. Clamped. A single scalar
 * store per field — safe from the HTTP thread while the radar thread runs
 * (worst case: one frame uses a just-changed value). */
void cluster_set_dbscan(RadarClusterer *c, double eps_m, int min_pts);

/* speed -> Doppler motion threshold (m/s); snr -> point strength gate (dB;
 * 0=off, static channel uses +3); fov -> azimuth half-angle (deg); doppler ->
 * merge velocity-coherence gate (m/s). Clamped. Same thread-safety note. */
void cluster_set_gates(RadarClusterer *c, double speed_min_mps, double snr_min_db,
                       double fov_half_deg, double doppler_gate_mps);

/* Current azimuth-gate half-angle (deg) — published on the wire for the wedge. */
double cluster_fov(const RadarClusterer *c);

/* Track one frame. `pts`/`n` are updated in place (each point's .tid is set to
 * its owning track id, or 255). `now_s` is a monotonic timestamp (seconds);
 * `dt` is seconds since the previous step. Emitted (confirmed, in-band) targets
 * are written to `out` up to `max_out`; returns the count. */
int cluster_step(RadarClusterer *c, RadarPoint *pts, int n,
                 double now_s, double dt,
                 RadarTarget *out, int max_out);

#endif /* AIRPOC_CLUSTER_H */
