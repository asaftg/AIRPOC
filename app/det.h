/* EO-detector consumer — the GUI couples to the standalone detection daemon (the
 * `detection/` module) over its served contract (detection/docs/INTEGRATION.md), never
 * its internals. A client thread subscribes to the daemon's SSE `/stream` on :8094,
 * keeps the latest det message, and the app re-broadcasts it to the browser on
 * `/det/stream` (and serves the latest one-shot on `/det`). Message schema is the
 * daemon's (dets[] classified boxes + movers[] motion-only, px in native 1440x1088) —
 * parsed in the front-end. */
#ifndef AIRPOC_DET_H
#define AIRPOC_DET_H

int  det_start(const char *host_port);   /* e.g. "127.0.0.1:8094"; NULL = default */
void det_stop(void);

/* Copy the latest raw det message JSON (NUL-terminated) into buf; returns its length
 * or 0 if no message yet. Thread-safe. */
int  det_get_frame_json(char *buf, int cap);
int  det_connected(void);        /* 1 while the daemon's SSE stream is up */

/* Block until a message newer than *last_seq arrives or timeout_ms elapses, then copy
 * it and update *last_seq. Returns length, or 0 on timeout. Powers the browser SSE push. */
int  det_wait_frame(unsigned *last_seq, char *buf, int cap, int timeout_ms);

/* Forward a raw control query to the daemon's /ctl (e.g. "conf=0.6"). The daemon owns
 * its knobs (conf/cadence/motion/max_dets/mot_k/mot_persist) and clamps server-side. */
void det_ctl(const char *daemon_query);

/* Copy the daemon's latest /stats JSON (health + the knobs object) for slider init +
 * readback. Returns length or 0. */
int  det_get_stats(char *buf, int cap);

#endif /* AIRPOC_DET_H */
