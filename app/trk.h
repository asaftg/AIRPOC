/* EO-tracker consumer — the GUI couples to the standalone EO tracker daemon (the
 * `eotrack/` module, `trackerd` on :8095) over its served contract
 * (eotrack/docs/INTEGRATION.md), never its internals. A client thread subscribes to the
 * daemon's SSE `/stream`, keeps the latest track message, and the app re-broadcasts it to
 * the browser on `/trk/stream` (latest one-shot on `/trk`). The tracker is the SINGLE
 * source of the EO boxes the operator sees — the raw detector boxes are dev-only. Message
 * schema is the daemon's (tracks[] with tid/state/cls/px/ang/lock, mode, engaged) — parsed
 * in the front-end. Structurally identical to det_client.c / radar_client.c. */
#ifndef AIRPOC_TRK_H
#define AIRPOC_TRK_H

int  trk_start(const char *host_port);   /* e.g. "127.0.0.1:8095"; NULL = default */
void trk_stop(void);

/* Copy the latest raw tracker message JSON (NUL-terminated) into buf; returns its length
 * or 0 if no message yet. Thread-safe. */
int  trk_get_frame_json(char *buf, int cap);
int  trk_connected(void);        /* 1 while the daemon's SSE stream is up */

/* Block until a message newer than *last_seq arrives or timeout_ms elapses, then copy
 * it and update *last_seq. Returns length, or 0 on timeout. Powers the browser SSE push. */
int  trk_wait_frame(unsigned *last_seq, char *buf, int cap, int timeout_ms);

/* Forward a raw control query to the daemon's /ctl (e.g. "engage=3"). The daemon owns its
 * knobs (engage/lock/gate_base/confirm/coast_s/clutter_s) and clamps server-side. */
void trk_ctl(const char *daemon_query);

/* Copy the daemon's latest /stats JSON (health + the knobs object) for readback. */
int  trk_get_stats(char *buf, int cap);

#endif /* AIRPOC_TRK_H */
