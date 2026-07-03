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
} RadarTarget;

/* A fully parsed + clustered frame, ready to serialise. */
typedef struct {
    uint32_t    frame_number;     /* chip frameNumber — gaps == dropped frames */
    int         n_points;
    int         n_targets;
    RadarPoint  points[RADAR_MAX_POINTS];
    RadarTarget targets[RADAR_MAX_TARGETS];
} RadarFrame;

#endif /* AIRPOC_RADAR_H */
