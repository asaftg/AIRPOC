/* source.h — frame source abstraction.
 *
 * Every frame the detector processes arrives through this vtable, so the exact
 * same pipeline runs whether frames come from the live EO shared-memory tap, a
 * recorded AIREC session, or a directory of 16-bit test images. Phase 1 ships
 * the tap source; replay sources land in phase 2 behind the same interface.
 *
 * A frame is one native IMX296 image: Y10 brightness packed in 16-bit LE words,
 * EO_IMG_W x EO_IMG_H, plus the tap metadata (timestamps, v4l2 seq, illuminator
 * state, driver drop counter).
 */
#ifndef DET_SOURCE_H
#define DET_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "airpoc_tap.h"   /* TAP_META_WORDS + tap_now_ns for the tap source */

typedef struct {
    const uint8_t *y10;         /* EO_FRAME_BYTES of Y10, valid until next next() */
    size_t         bytes;
    int            w, h;
    uint64_t       seq;         /* tap record sequence (monotonic on this source) */
    uint64_t       t_src_ns;    /* exposure-referenced CLOCK_MONOTONIC (authoritative) */
    uint64_t       t_pub_ns;
    uint32_t       meta[TAP_META_WORDS];
    uint64_t       gap_before;  /* frames lost immediately before this one */
} DetFrame;

struct FrameSource;
typedef struct FrameSource FrameSource;

/* Vtable. next() returns 1 = frame ready, 0 = none right now (caller may idle),
 * -1 = source gone (caller should close + reopen). Implementations never block
 * longer than a short poll. */
struct FrameSource {
    int  (*next)(FrameSource *s, DetFrame *out);
    void (*close)(FrameSource *s);
    int  (*connected)(const FrameSource *s);
    void *impl;
};

/* Live EO tap. Returns NULL on allocation failure; a not-yet-present publisher
 * is NOT a failure — the source reports connected()==0 and retries internally. */
FrameSource *tap_source_open(const char *tap_name);

#endif /* DET_SOURCE_H */
