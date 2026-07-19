# Tap protocol v1 — shm slot rings

A tap is a module's **documented output** for full-rate data HTTP cannot carry
(the EO native stream is ~188 MB/s). One publisher, any number of readers, and
the publisher never blocks: fixed-size slots, overwrite-oldest, per-slot seqlock.
Readers drain losslessly while they keep up and see laps as counted gaps.

Single header: `recorder/tap/airpoc_tap.h`. Producers vendor a copy next to their
own code; there is no link dependency. Protocol drift is loud — the magic/version
handshake fails at `tap_open`, so a mismatched producer cannot corrupt a reader
silently.

## Live rings

All six taps below are live, plus one recorder-internal channel. Geometry is set
by the **publisher**, not by the recorder.

| ring | slots | payload cap | publisher | carries |
|---|---|---|---|---|
| `airpoc.eo_y10` | 16 | sizeimage (3,133,440) | `eo/pipeline/libeo.c` | native Y10; the capture DMA writes straight into the slot |
| `airpoc.eo_jpeg` | 16 | 1 MiB | `eo/pipeline/mjpeg.c` | the display JPEG the operator actually saw |
| `airpoc.radar_raw` | 512 | 8 KiB | `radar/src/main.c` | every UART `read()`, published before parsing |
| `airpoc.radar_wire` | 16 | 256 KiB | `radar/src/main.c` | radar frame JSON (the SSE payload) |
| `airpoc.radar_cli` | 64 | 8 KiB | `radar/src/main.c` | chip CLI telemetry (~1 Hz). Exists on no other path — unrecorded means gone |
| `airpoc.det_wire` | 16 | 128 KiB | `detection/src/main.c` | detector frame JSON |
| *(events)* | — | — | recorder-internal | 5 Hz stats/events the recorder writes itself; not a tap |

> **Usable depth is `n_slots - 2`, not `n_slots`.** `tap_read` keeps a slot of
> margin so the slot being written is never handed to a reader. The 16-slot EO
> rings therefore buffer 14 frames — **~233 ms at 60 Hz**, not 266 ms.

## Publisher checklist (for a module adding a tap)

1. `tap_create(&t, "airpoc.<chan>", n_slots, slot_bytes, meta_json)` at start.
   Failure → log once and keep running (a recorder-less system is fine).
2. Hot path, either:
   - zero-copy: `p = tap_slot_begin(&t)` → write payload into `p` →
     `tap_slot_commit(&t, len, t_src_ns, meta, 0)`. This is how `eo_y10`
     retargets its one existing DMA memcpy, or
   - copy: `tap_write(&t, buf, len, t_src_ns, meta)` for small payloads.
3. `t_src_ns` = CLOCK_MONOTONIC of the *source event* (the V4L2 buffer timestamp,
   the UART read return) — not "now" at publish time.
4. `tap_destroy` on shutdown.
5. Declare the tap in the module README: name, slot geometry, meta[6] schema.

## Memory model (why readers cannot see torn data)

Writer: `seq_begin = S+1` (then a full fence, so it is globally visible before
any payload byte) → payload + fields → `seq_end = S+1` release → `wseq = S+1`
release. Reader: check `seq_end == S+1` → copy/transform → acquire fence →
re-check `seq_begin == S+1`; a mismatch means lapped mid-read → drop and count.

> Pitfall: readers must treat `payload_len > slot cap` (the TRUNCATED flag) and
> lap jumps as normal — both are counted, neither is an error.

## Re-attach — surviving a tap deleted underneath the reader

If a publisher restarts, or anything deletes `/dev/shm/airpoc.*` while a reader
is attached, the old mapping stays valid but orphaned. Without a check the reader
would go on reading a dead copy forever — recording nothing while every counter
still looked healthy. This has happened in the field, so the recorder detects it.

Each channel stores the shm identity (`st_dev`/`st_ino`) at open. `tap_stale()`
re-stats the path and compares. `tap_recheck()` runs at **1 Hz** per channel, and
on a mismatch it closes the orphan, increments `tap_reattach`, logs, writes a
`tap_reattach <chan>` **marker event into the recording** so the hole is visible
offline, then re-opens.

How to read it:

- `/stats` and the session manifest carry `tap_reattach` per channel. **Non-zero
  means the feed was swapped mid-recording and there is a gap at that marker.**
- A tap that never attached at all shows `connected:0` instead — a different
  condition, and not one re-attach can fix.

Implementation: `recorder/tap/airpoc_tap.h` (`tap_stale`), `recorder/src/channel.c`
(`tap_recheck`).

## History

The EO and radar work orders that used to occupy this file are done and have been
removed. Radar taps landed 2026-07-05; the EO `eo_y10` / `eo_jpeg` taps and the
V4L2 timestamp/sequence plumbing landed with the C pipeline; the detector and
radar-CLI taps followed. This document describes the protocol as it stands — it
is not a worklist.
