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

/* Forward host-side DBSCAN cluster cfg to the daemon (best-effort GET /ctl). Needs a
 * daemon control endpoint — see the request in radar/docs/INTEGRATION.md follow-up;
 * a no-op if the daemon doesn't implement it yet. */
void radar_set_tune(double cluster_eps_m, int min_points);

#endif /* AIRPOC_RADAR_H */
