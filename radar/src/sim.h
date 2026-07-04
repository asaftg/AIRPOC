/* Synthetic TLV frame generator — lets the whole module (parser, clusterer,
 * previewer, HTTP contract) run with NO board attached. Enabled with `-s`.
 * It emits real mmw_demo TLV bytes through the SAME parser path as the
 * radio, so it exercises everything the hardware would, and gives the GUI
 * agent a live, moving PPI to develop against while the Jetson is off.
 *
 * Scene: a person walking across at ~30 m, a vehicle receding at ~120 m,
 * plus a little static clutter. */
#ifndef AIRPOC_SIM_H
#define AIRPOC_SIM_H

#include <stddef.h>
#include <stdint.h>

/* Build one synthetic mmw_demo packet into buf; returns its byte length
 * (0 if it would not fit). `t_s` is a monotonic time used to animate the
 * scene; `frame_no` is stamped into the header. */
size_t sim_build_frame(uint8_t *buf, size_t cap, uint32_t frame_no, double t_s);

#endif /* AIRPOC_SIM_H */
