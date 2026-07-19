# recorder — record & replay

Records EVERYTHING the payload produces — native EO, radar, metadata, future
channels — onto the NVMe with near-zero cost to the live pipeline, and replays
any session through the operator console exactly as it looked live.

## I/O contract

**Consumes**
- shm taps (protocol v1, [docs/TAP.md](docs/TAP.md)): `airpoc.radar_raw`
  (bit-perfect UART bytes, ✅ live), `airpoc.radar_wire` (the exact SSE
  frame JSON, ✅ live), `airpoc.eo_y10` (native Y10 + frame-synchronous AE
  meta, ✅ live), `airpoc.eo_jpeg` (the display JPEGs the operator saw, ✅ live),
  `airpoc.det_wire` (the EO-detector frame JSON, verbatim, ✅ live —
  replays through `/replay/det`), and `airpoc.radar_cli` (the radar chip's
  CLI telemetry, raw ASCII ~1 Hz — recorded for offline parsing, not
  replayed). Absent taps degrade to `connected:0` in
  `/stats`; a session records whatever channels exist and replays exactly that.
- The modules' documented `/stats` surfaces (`:8091`, `:8092`, `:8080`),
  polled at 5 Hz into the events channel.

**Produces**
- AIREC v1 sessions under `/data/recordings/<sid>/` ([docs/FORMAT.md](docs/FORMAT.md)).
- HTTP on **:8093** — recording control, library, replay
  ([docs/REPLAY.md](docs/REPLAY.md), GUI handoff in
  [docs/GUI_INTEGRATION.md](docs/GUI_INTEGRATION.md)). The console proxies it
  as `/rec/<path>`. Replay serves the recorded channels at their live rates
  (radar/detections over SSE), a denoised + seekable **H.264 native movie** per
  session (on-demand HD convert, version-stamped), and a streaming `.tar`
  **offload** ([docs/OFFLOAD.md](docs/OFFLOAD.md)).

## Layout

| file | role |
|---|---|
| `tap/airpoc_tap.h` | tap protocol v1, vendored by producer modules |
| `src/channel.c` | drain threads, single-pass 16→10 pack, O_DIRECT chunk writers |
| `src/session.c` | recording→pending→saved/discarded lifecycle, crash recovery, disk guard |
| `src/replay.c` | timeline clock, MJPEG pushers, recorded-endpoint views, video-source select, EO-drift check |
| `src/render.c` `src/eo_tonemap.c` | native-frame reconstruction (unpack + tone map + JPEG); the tone map itself |
| `src/transcode.c` | cached H.264 of a session's native replay (smooth play over WiFi) |
| `src/library.c` `src/thumbs.c` | /library filters, 8-still previews |
| `src/store.c` `src/http.c` `src/events.c` `src/disk.c` `src/pack10.c` | manifest store, :8093 server (Range-capable), stats poller, disk guard, pack kernel |
| `systemd/` | `airpoc-recorder.service` + `install.sh` |
| `tools/` | `tap_bench.c` (synthetic soak), `airec_dump.py` (verify), `verify_replay_match.py` (native-vs-live tone-map check), `offload_pull.sh` + `airpoc-offload.ps1` (pull), `compress_native.sh` |

**Tone map (`src/eo_tonemap.c`):** native replay renders through the recorder's
own copy of the EO feed's tone map, kept byte-identical to
`eo/pipeline/isp.c:isp_scale_tonemap`. Self-contained — the recorder does not
reach into the eo/ module. Drift is caught two ways: an automatic per-open
comparison of a native frame against the operator's recorded display frame
(`tonemap_vs_eo` in `/replay/state`), plus a `tonemap_hash` stamped per
recording. See
[docs/REPLAY.md](docs/REPLAY.md#keeping-replay-identical-to-the-live-feed--and-knowing-if-it-isnt).

## Build / run

```
cd recorder/src && make          # airpoc_recorder + tap_bench (build on the Jetson)
./airpoc_recorder [-p 8093] [-r /data/recordings]
sudo systemd/install.sh          # persistent service
```

NVMe provisioning: `jetson/nvme/` (once per device).

## Budget (measured on the Orin Nano Super)

Full rate — Y10 1440×1088@60 packed 10-bit + display JPEGs + radar + events:
**~125 MB/s to disk, zero drops**, recorder CPU ≈ a fraction of one core.
1 TB NVMe ≈ 2 h. `mode=y8` (~338 GB/h) and `keep=N` decimation are levers;
the display-JPEG channel is always on regardless.

> Pitfall: producers never wait for the recorder — rings overwrite oldest. If
> the NVMe stalls, records are dropped **and counted** (`drops_ring` /
> `drops_queue` in `/stats`, `GAP_BEFORE` flags on disk).
>
> **A clean `drops_*` does NOT mean the recording is complete.** A failed or
> short write increments `write_errors`; a wrong-sized record increments
> `bad_size`. Neither touches `drops_*`, and `records`/`bytes` keep climbing
> either way. The complete-recording test is all four at zero:
> `drops_ring + drops_queue + write_errors + bad_size == 0`.

> Pitfall: sessions land as `pending` on REC-stop and are auto-purged after
> 24 h unless saved. Saved sessions are never auto-deleted; `purge_native=`
> is the explicit space-reclaim (drops raw Y10, keeps everything else).

> Loss watchdog: the recorder can't record frames a producer never publishes,
> but it never loses a feed *silently*. If a tap-fed channel stops producing
> >2 s mid-recording, `/stats` sets that channel's `lost:1` and a `channel_lost`
> event (with the last-frame time) is written into the recording. See
> [docs/FORMAT.md](docs/FORMAT.md) events.
