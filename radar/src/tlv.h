/* mmw_demo TLV framing + parser for the AWR2944P (DDM variant).
 *
 * Port of the ground bench's radar/tlv_parser.py. Byte-level critical
 * path: find magic word, read the 32-byte header, walk numTLVs, extract
 * DetectedPoints (TLV 1) and SideInfo (TLV 7) when present, and resync
 * on the next magic word if totalPacketLen disagrees — radar streams get
 * corrupted by USB hiccups and cable bumps, and resync costs at most one
 * frame, never the session. Tracker TLVs (308/309) are recognised so a
 * future gtrack-linked firmware drops in without a parser rewrite. */
#ifndef AIRPOC_TLV_H
#define AIRPOC_TLV_H

#include "radar.h"

/* Wire magic word: 02 01 04 03 06 05 08 07 (little-endian u64). */
extern const uint8_t TLV_MAGIC[8];

/* Streaming, resyncing parser. Feed raw UART bytes; get complete frames
 * via the callback. Not thread-safe — own it from a single reader. */
typedef struct TLVStream TLVStream;

TLVStream *tlv_stream_new(void);
void       tlv_stream_free(TLVStream *s);

/* Callback invoked once per complete, sanity-checked frame. `points`/
 * `n_points` are the parsed DetectedPoints (with SideInfo applied if the
 * frame carried it); `frame_number` is the chip's frameNumber. `stats` is the
 * chip's per-frame timing (TLV 6) or NULL if that frame carried no stats TLV. */
typedef void (*tlv_frame_cb)(void *user, uint32_t frame_number,
                             const RadarPoint *points, int n_points,
                             const RadarStats *stats);

/* Append `n` bytes and drain all complete frames, invoking `cb` for each. */
void tlv_stream_feed(TLVStream *s, const uint8_t *data, size_t n,
                     tlv_frame_cb cb, void *user);

#endif /* AIRPOC_TLV_H */
