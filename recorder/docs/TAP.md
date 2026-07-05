# Tap protocol v1 — shm slot rings

A tap is a module's **documented output** for full-rate data HTTP can't carry
(the EO native stream is 188 MB/s). One publisher, any readers, publisher never
blocks: fixed-size slots, overwrite-oldest, per-slot seqlock. Readers drain
losslessly while they keep up and detect laps as counted gaps.

Single header: `recorder/tap/airpoc_tap.h` (vendored by producers; no link dep).

## Publisher checklist (for a module adding a tap)

1. `tap_create(&t, "airpoc.<chan>", n_slots, slot_bytes, meta_json)` at start.
   Failure → log once, keep running without it (a recorder-less system is fine).
2. Hot path, either:
   - zero-copy: `p = tap_slot_begin(&t)` → write payload into `p` →
     `tap_slot_commit(&t, len, t_src_ns, meta, 0)` — this is how eo_y10
     retargets its one existing DMA memcpy, or
   - copy: `tap_write(&t, buf, len, t_src_ns, meta)` for small payloads.
3. `t_src_ns` = CLOCK_MONOTONIC of the *source event* (V4L2 buffer timestamp,
   UART read return), not "now" at publish.
4. `tap_destroy` on shutdown.
5. Declare the tap in the module README: name, slot geometry, meta[6] schema.

## Live rings

| ring | payload cap | slots | state | note |
|---|---|---|---|---|
| `airpoc.eo_y10` | sizeimage (3,133,440) | 16 | pending WI-EO-2 | 266 ms depth at 60 Hz |
| `airpoc.eo_jpeg` | 1 MiB | 16 | pending WI-EO-3 | display JPEG after encode; 1 MiB covers the NATIVE display res |
| `airpoc.radar_raw` | 8 KiB | 512 | ✅ live | every UART read() |
| `airpoc.radar_wire` | 256 KiB | 16 | ✅ live | SSE frame JSON |

Producers vendor a copy of the header next to their code (`radar/tap/airpoc_tap.h`
is the precedent); protocol drift is loud — the magic/version handshake fails
at `tap_open`, it can't corrupt silently.

## Memory model (why readers can't see torn data)

Writer: `seq_begin = S+1` (then a full fence, so it's globally visible before
any payload byte) → payload + fields → `seq_end = S+1` release → `wseq = S+1`
release. Reader: check `seq_end == S+1` → copy/transform → acquire fence →
re-check `seq_begin == S+1`; mismatch = lapped mid-read → drop + count.

> Pitfall: readers must treat `payload_len > slot cap` (TRUNCATED flag) and lap
> jumps as normal — both are counted, neither is an error.

## Work orders

**Radar owner — DONE 2026-07-05.** Both taps live and HW-verified: 26.3 Hz
byte-verbatim wire frames, every UART read captured, 0 drops / 0 gaps / 100%
CRC on a 45 s session. Header vendored at `radar/tap/airpoc_tap.h`; taps
declared in `radar/README.md` + `docs/INTEGRATION.md`.

**EO owner (`eo/pipeline/`) — pending**
- WI-EO-1: `capture.c cap_dqbuf` — stop discarding `b.timestamp`/`b.sequence`
  (add to the Capture struct; they are the eo_y10 `t_src_ns` and `meta[0]`).
- WI-EO-2: `libeo.c` — `tap_create("airpoc.eo_y10", 16, sizeimage)` in
  `eo_start`; in `cap_thread`, take the frame pointer from `tap_slot_begin()`
  so the existing DMA memcpy IS the publish; `tap_slot_commit` with
  {v4l2_seq, exp_lines, gain, vmax, mean×100, drops}. `consume_frame` and the
  tone handoff keep the same pointer; heap fallback if shm fails. `eo.h` frozen.
- WI-EO-3: `mjpeg.c mjpeg_publish` — one `tap_write("airpoc.eo_jpeg", …)` of
  the just-encoded JPEG with {seq, dw, dh, zoom, res_idx}. Slots 16 × 1 MiB —
  sized for the NATIVE display res on noisy night frames.
- WI-EO-4: declare both taps in `eo/pipeline/README.md` + `INTEGRATION.md`.

**Radar owner (`radar/src/`)**
- WI-RD-1: `main.c` — `tap_write("airpoc.radar_raw", buf, n, t_read)` after
  every successful `read()` at BOTH feed sites (cfg-settle peek + main loop),
  BEFORE `tlv_stream_feed` so capture is independent of parse health.
- WI-RD-2: `main.c on_frame` — `tap_write("airpoc.radar_wire", json, len,
  now)` with {frameNumber, n_points, n_targets} right after `wire_frame_json`.
- WI-RD-3: declare both taps in `radar/README.md` + `docs/INTEGRATION.md`.
