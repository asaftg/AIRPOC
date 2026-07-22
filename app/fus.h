/* Fusion consumer — the GUI couples to the standalone fusion daemon (the `fusion/` module,
 * `fusiond` on :8096) over its served contract (fusion/docs/INTEGRATION.md), never its
 * internals. A client thread subscribes to the daemon's SSE `/stream`, keeps the latest fused
 * message, and the app re-broadcasts it to the browser on `/fus/stream` (latest one-shot on
 * `/fus`, health + knobs on `/fstats`).
 *
 * Fusion joins the EO tracker and radar tracker streams into ONE target picture: a fused row
 * carries radar range + EO angles/class under a fusion-assigned global id, and unmatched
 * targets pass through source-tagged. A constituent never appears twice, so consumers render
 * `targets[]` verbatim — the console does NO client-side dedup.
 *
 * Fusion is OPTIONAL: with it down the console falls back to the per-sensor lists, and the
 * radar -> gimbal EO-blind chain never depends on it at all. Structurally identical to
 * trk_client.c / det_client.c / radar_client.c. */
#ifndef AIRPOC_FUS_H
#define AIRPOC_FUS_H

int  fus_start(const char *host_port);   /* e.g. "127.0.0.1:8096"; NULL = default */
void fus_stop(void);

/* Copy the latest raw fusion message JSON (NUL-terminated) into buf; returns its length
 * or 0 if no message yet. Thread-safe. */
int  fus_get_frame_json(char *buf, int cap);
int  fus_connected(void);        /* 1 while the daemon's SSE stream is up */

/* Block until a message newer than *last_seq arrives or timeout_ms elapses, then copy
 * it and update *last_seq. Returns length, or 0 on timeout. Powers the browser SSE push. */
int  fus_wait_frame(unsigned *last_seq, char *buf, int cap, int timeout_ms);

/* Forward a raw control query to the daemon's /ctl (e.g. "trim_az=0.4"). Fusion owns the
 * radar<->EO mount trim plus its association knobs, and clamps server-side. */
void fus_ctl(const char *daemon_query);

/* Copy the daemon's latest /stats JSON (health + the knobs object) for readback. */
int  fus_get_stats(char *buf, int cap);

#endif /* AIRPOC_FUS_H */
