/* AIRPOC radar module — shared types and tunables.
 *
 * One TI AWR2944PEVM (77 GHz, 4TX/4RX), NO DCA1000. Data leaves the chip
 * as the mmw_demo TLV point cloud over the data UART; config is pushed
 * over the CLI UART. This module parses that stream with zero frame loss,
 * clusters it into class-less target boxes, and serves a PPI previewer.
 *
 * Coordinate frame (sensor): +x right, +y forward (boresight), +z up.
 * Everything downstream is class-less — person/vehicle labelling is the
 * fusion module's job (see docs/SYSTEM_OVERVIEW.md). */
#ifndef AIRPOC_RADAR_H
#define AIRPOC_RADAR_H

#include <stdint.h>
#include <stddef.h>

#define RADAR_MAX_POINTS   1024   /* generous; mmw_demo gives a few hundred */
#define RADAR_MAX_TARGETS   64

/* One detected point, sensor frame. snr/noise carry per-point SNR in dB (live
 * via TLV 7 SideInfo); NaN only if a firmware without SideInfo is flashed. */
typedef struct {
    float x, y, z;        /* metres */
    float doppler;        /* m/s, +approaching per TI convention sign of x-corr */
    float snr, noise;     /* dB — per-point SNR (live via TLV 7); NaN if a
                             firmware without SideInfo is ever flashed */
    float range;          /* m */
    float az, el;         /* degrees */
    int   tid;            /* assigned cluster/track id, 255 = unassigned */
} RadarPoint;

/* One published class-less target — a cluster detected this frame (no
 * coasting; ids kept stable frame-to-frame via association). */
typedef struct {
    int   tid;
    float x, y, z;                /* centroid, m */
    float vx, vy, vz;             /* velocity, m/s */
    float sx, sy, sz;             /* half-extents, m */
    float conf;                   /* 0..1 */
    int   num_points;             /* hits accumulated */
    int   suspect;                /* 1 = standing in a stronger target's
                                     co-range co-velocity shadow (possible
                                     antenna-sidelobe copy, not yet held long
                                     enough to suppress) — fusion should
                                     treat with caution */
    int   mv_class;               /* walk-guard motion class: 0 = UNVERIFIED_SLOW
                                     (radially quiet, not judged), 1 = VERIFIED_MOVER
                                     (claimed doppler matches displacement, or
                                     coherent cross motion), 2 = SUSPECT (claimed
                                     motion contradicted; on the way out) */
} RadarTarget;

/* Per-frame timing the chip measures and reports in the stats TLV (type 6,
 * MmwDemo_output_message_stats). This is the ground truth for "how fast can we
 * run the frame": the DSP must finish interframe_proc within the inter-frame
 * gap (period - active). interframe_margin is the chip's own spare-time figure;
 * it going toward zero is what caps the frame rate, not any duty-cycle guess. */
typedef struct {
    double interframe_proc_us;    /* DSP range/Doppler/CFAR/AoA time per frame */
    double transmit_out_us;       /* time to ship the output TLVs */
    double interframe_margin_us;  /* spare time before the next frame (chip-reported) */
    double interchirp_margin_us;  /* spare time between chirps */
    double active_cpu_pct;        /* CPU load during active (chirping) phase */
    double interframe_cpu_pct;    /* CPU load during inter-frame phase */
} RadarStats;

/* A fully parsed + clustered frame, ready to serialise. */
typedef struct {
    uint32_t    frame_number;     /* chip frameNumber — gaps == dropped frames */
    int         n_points;
    int         n_targets;
    RadarPoint  points[RADAR_MAX_POINTS];
    RadarTarget targets[RADAR_MAX_TARGETS];
} RadarFrame;

#endif /* AIRPOC_RADAR_H */
