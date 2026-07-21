/* eo_reader.h - reads the newest raw Y10 frame from the airpoc.eo_y10 tap. Wraps
 * AirTapSub with the same reopen-on-stale recovery the detector uses (an EO restart
 * unlinks+recreates the shm; a stale mapping just stops advancing). Drains to the
 * freshest frame each call - the tracker never wants a backlog, only "now".
 * Frames are 16-bit LE words (Y10); NCC is intensity-affine invariant so the raw
 * words are used directly, no shift needed. */
#ifndef TRK_EO_READER_H
#define TRK_EO_READER_H

#include <stdint.h>
#include <stddef.h>

typedef struct EoReader EoReader;

EoReader *eo_open(const char *tap_name);
void      eo_close(EoReader *r);
int       eo_connected(const EoReader *r);

/* Copy the newest available frame into buf (>= EO_FRAME_BYTES). Returns 1 if a
 * frame was copied (fills w,h,seq,t_src_ns,meta[6]), 0 if none new / not attached. */
int eo_latest(EoReader *r, uint16_t *buf, size_t cap,
              int *w, int *h, uint64_t *seq, uint64_t *t_src_ns, uint32_t meta[6]);

#endif /* TRK_EO_READER_H */
