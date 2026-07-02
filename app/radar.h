/* Radar handoff — the contract between the radar module and the GUI, modeled on the
 * Seeker AWR2944P wire shape (point cloud + clustered target boxes on a limited-
 * azimuth sector). World coords are metres in the sensor frame: +x = right,
 * +y = forward (boresight). Until the real AWR module lands, radar_stub.c provides a
 * synthetic source so the scope renders live; swapping in the real reader is a
 * link-time change (implement radar_start/stop/get_latest). */
#ifndef AIRPOC_RADAR_H
#define AIRPOC_RADAR_H

#define RADAR_MAX_POINTS  192
#define RADAR_MAX_TARGETS 12

typedef struct { float x, y, v, snr; } radar_point_t;               /* m, m, m/s, dB */
typedef struct { int tid; float x, y, vx, vy, sx, sy, conf; } radar_target_t;

typedef struct {
    int   connected;
    float max_range_m;
    float fov_half_deg;        /* half azimuth of the useful sector (e.g. 60)        */
    int   num_points, num_targets;
    radar_point_t  points[RADAR_MAX_POINTS];
    radar_target_t targets[RADAR_MAX_TARGETS];
} radar_frame_t;

int  radar_start(const char *port);   /* 0 on success; optional device */
void radar_stop(void);
/* Copy the most-recent frame into *out. Returns 1 if connected + filled, else 0. */
int  radar_get_latest(radar_frame_t *out);

#endif /* AIRPOC_RADAR_H */
