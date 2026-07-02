/* Serialise a RadarFrame to the JSON wire shape the previewer (and, later,
 * the GUI/fusion) consumes. Shape mirrors the ground bench's
 * sensor_bridge.radar_to_wire so the GUI agent can consume it unchanged:
 *   { connected, frame_id, timestamp, profile, max_range_m, fov_half_deg,
 *     num_points, num_targets,
 *     points:  [{x,y,z,v,snr,r,az,el,tid}, ...],
 *     targets: [{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,coasting,class}, ...] }
 * SNR that is NaN (firmware without SideInfo) serialises as null. */
#ifndef AIRPOC_WIRE_H
#define AIRPOC_WIRE_H

#include "radar.h"

int wire_frame_json(char *buf, size_t cap, const RadarFrame *f,
                    double timestamp, double max_range_m, double fov_half_deg,
                    const char *profile);

#endif /* AIRPOC_WIRE_H */
