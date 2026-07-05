# recorder — record & replay

Records EVERYTHING the payload produces — native EO, radar, metadata, future
channels — onto the NVMe with near-zero cost to the live pipeline, and replays
any session through the operator console exactly as it looked live.

## I/O contract

**Consumes**
- shm taps (protocol v1, [docs/TAP.md](docs/TAP.md)): `airpoc.eo_y10` (native
  Y10 frames + frame-synchronous AE meta), `airpoc.eo_jpeg` (the display JPEGs
  the operator saw), `airpoc.radar_raw` (bit-perfect UART bytes),
  `airpoc.radar_wire` (the exact SSE frame JSON). Absent taps degrade to
  `connected:0` in `/stats` — nothing crashes.
- The modules' documented `/stats` surfaces (`:8091`, `:8092`, `:8080`),
  polled at 5 Hz into the events channel.

**Produces**
- AIREC v1 sessions under `/data/recordings/<sid>/` ([docs/FORMAT.md](docs/FORMAT.md)).
- HTTP on **:8093** — recording control, library, replay
  ([docs/REPLAY.md](docs/REPLAY.md), GUI handoff in
  [docs/GUI_INTEGRATION.md](docs/GUI_INTEGRATION.md)). The console proxies it
  as `/rec/<path>`.

## Layout

| file | role |
|---|---|
| `tap/airpoc_tap.h` | tap protocol v1, vendored by producer modules |
| `src/channel.c` | drain threads, single-pass 16→10 pack, O_DIRECT chunk writers |
| `src/session.c` | recording→pending→saved/discarded lifecycle, crash recovery, disk guard |
| `src/replay.c` | timeline clock, MJPEG pushers, recorded-endpoint views |
| `src/library.c` `src/thumbs.c` | /library filters, 8-still previews |
| `src/store.c` `src/http.c` `src/events.c` `src/disk.c` `src/pack10.c` | manifest store, :8093 server, stats poller, disk guard, pack kernel |
| `tools/` | `tap_bench.c` (synthetic soak publisher), `airec_dump.py` (verify), `offload_pull.sh`, `compress_native.sh` |
| `systemd/` | `airpoc-recorder.service` + `install.sh` |

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
> the NVMe stalls >0.8 s, records are dropped **and counted** (`drops_*` in
> `/stats`, `GAP_BEFORE` flags on disk). Silence in those counters means the
> recording is complete.

> Pitfall: sessions land as `pending` on REC-stop and are auto-purged after
> 24 h unless saved. Saved sessions are never auto-deleted; `purge_native=`
> is the explicit space-reclaim (drops raw Y10, keeps everything else).
