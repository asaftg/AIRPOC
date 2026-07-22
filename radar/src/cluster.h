/* Temporal multi-target tracker for the AWR2944P point cloud.
 *
 * Replaces the earlier per-frame DBSCAN+Kalman (ground-bench clustering.py port).
 * Class-agnostic track-before-detect: velocity from position history (range-rate
 * / angle-rate, never ambiguous Doppler); two-tier nearest-track association
 * (confirmed tracks claim points first — junk cannot shred an established
 * target); a moving channel plus a fresh-static occupancy channel so a car that
 * drives in and parks keeps its box; fast M-of-N confirm; short coast; spatial
 * dedup so co-located tracks emit one box.
 *
 * Post-confirmation CONSISTENCY GUARD (2026-07-11): a confirmed track must stay
 * physically coherent to live and must earn emission with positive evidence.
 * KILL is reserved for overall incoherence — unphysical path speed, re-latch
 * teleports, or domain incoherence on a track with no coherent progress in
 * EITHER domain (this is what ended the immortal wandering garage track and
 * the 16 dB ghost storms). A coherent directed mover is never killed for
 * jitter or for flutter in one domain (measurement extent / clutter passing
 * through its gate) — those only UNLATCH its emission until it looks clean
 * again, and a track re-earns a previously-held latch cheaply, including in
 * place (walk-then-stand keeps its box). Emission also needs brightness: the
 * lifetime peak MOVING-point SNR over a range-dependent bar (floor noise is
 * 16-21 dB everywhere; a real close target is far brighter, R^4; at range the
 * bar gains a margin because it sits at the floor). A faint far track that
 * cannot clear the bar may emit only on a full coherent streak PLUS doppler
 * self-consistency (claimed doppler must match its own fitted range-rate —
 * spur streaks fail this by an order of magnitude). During a near-field flood
 * (something moving right next to the radar lights the whole sidelobe
 * hemisphere) close tracks can not earn evidence at all. All thresholds are
 * range-aware, and the guard only judges frames with fresh measurements.
 *
 * Validated offline against the fixture corpus; parity vs the Python reference
 * via radar/tools/track_replay.c + parity_check.py (see those for the corpus
 * results and the FP-chaos caveat).
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
 *   ELEV         -> elevation gate (input; 90 = off)
 *   DOPPLER      -> velocity-coherence gate for the duplicate-merge
 *   CONFIRM      -> M-of-N hits to confirm a track (latency vs false alarms)
 *   COAST        -> seconds a confirmed track survives a dropout
 *   PARK         -> seconds a moved-then-stopped track is held
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
#define CLUSTER_DEFAULT_ELMAX    20.0   /* deg — elevation half-angle gate. Default =
                                         * the antenna's physical elevation beam edge:
                                         * beyond ~±20 the array has no real gain, so
                                         * reports there are angle-noise / multipath.
                                         * Beam moves with the radar → gimbal-safe.
                                         * Operator widens to 90 (= off) via /ctl. */
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
#define CLUSTER_ELMAX_MIN        5.0
#define CLUSTER_ELMAX_MAX        90.0
#define CLUSTER_DOP_MIN          0.5
#define CLUSTER_DOP_MAX          20.0
/* Temporal-track knobs. */
#define CLUSTER_DEFAULT_CONFIRM  3      /*     — M-of-N fast-confirm hits (window = M+1) */
#define CLUSTER_DEFAULT_COAST_S  0.4    /* s   — a confirmed track survives a dropout this long */
#define CLUSTER_DEFAULT_PARK_S   0.0    /* s   — a moved-then-stopped (parked) track is held this long */
#define CLUSTER_CONFIRM_MIN      1
#define CLUSTER_CONFIRM_MAX      6
#define CLUSTER_COAST_MIN        0.0
#define CLUSTER_COAST_MAX        3.0
#define CLUSTER_PARK_MIN         0.0
#define CLUSTER_PARK_MAX         60.0

RadarClusterer *cluster_new(void);
void            cluster_free(RadarClusterer *c);

/* eps -> dedup radius (m); min_pts -> seed points. Clamped. A single scalar
 * store per field — safe from the HTTP thread while the radar thread runs
 * (worst case: one frame uses a just-changed value). */
void cluster_set_dbscan(RadarClusterer *c, double eps_m, int min_pts);

/* speed -> Doppler motion threshold (m/s); snr -> point strength gate (dB;
 * 0=off, static channel uses +3); fov -> azimuth half-angle (deg); elmax ->
 * elevation half-angle (deg, 90 = off); doppler -> merge velocity-coherence
 * gate (m/s). Clamped. Same thread-safety note. */
void cluster_set_gates(RadarClusterer *c, double speed_min_mps, double snr_min_db,
                       double fov_half_deg, double el_max_deg, double doppler_gate_mps);

/* confirm_m -> M-of-N fast-confirm hits (window = M+1); coast_s -> seconds a
 * confirmed track coasts through a dropout before it dies; park_s -> seconds a
 * moved-then-stopped track is held. Clamped. Same thread-safety note. */
void cluster_set_track(RadarClusterer *c, int confirm_m, double coast_s, double park_s);

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
