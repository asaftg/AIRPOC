/* Radar consumer — the GUI couples to the standalone radar daemon (the `radar/`
 * module) over its SSE contract (radar/docs/INTEGRATION.md), never its internals.
 * A client thread subscribes to the daemon's `/stream` on :8092, keeps the latest
 * frame JSON, and the app serves it verbatim on `/radar` so the browser stays
 * single-origin. The frame schema is the daemon's (points/targets in the sensor
 * frame, metres) — parsed in the front-end. */
#ifndef AIRPOC_RADAR_H
#define AIRPOC_RADAR_H

int  radar_start(const char *host_port);   /* e.g. "127.0.0.1:8092"; NULL = default */
void radar_stop(void);

/* Copy the latest raw daemon frame JSON (NUL-terminated) into buf; returns its length
 * or 0 if no frame yet. Thread-safe. */
int  radar_get_frame_json(char *buf, int cap);
int  radar_connected(void);      /* 1 if the daemon reports a connected radar */
int  radar_num_targets(void);    /* target count in the latest frame (for /stats)  */

/* Block until a frame newer than *last_seq arrives (event-driven; the frame thread
 * signals on every new frame) or timeout_ms elapses, then copy it into buf and update
 * *last_seq. Returns the frame length, or 0 on timeout. Lets the app PUSH radar to the
 * browser over SSE at the sensor's native rate instead of the browser polling. */
int  radar_wait_frame(unsigned *last_seq, char *buf, int cap, int timeout_ms);

/* Forward a raw control query to the daemon's /ctl (e.g. "eps=8&fov=60"). The daemon
 * owns all six live controls (eps/minpts/speed/snrmin/fov/doppler) and clamps them
 * server-side, so the GUI just forwards and reads back. Best-effort GET /ctl. */
void radar_ctl(const char *daemon_query);

/* Copy the daemon's latest /stats JSON (fps, drops, counts, and the current value of
 * all six controls) into buf for slider init + readback. Returns length or 0. */
int  radar_get_stats(char *buf, int cap);

#endif /* AIRPOC_RADAR_H */
