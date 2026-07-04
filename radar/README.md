# Radar module

TI **AWR2944PEVM** (77 GHz, 4TX/4RX), **no DCA1000**. The chip runs our
mmw_demoDDM firmware and emits a TLV point cloud over UART; this module
pushes the profile, parses that stream with **zero frame loss**, clusters it
into **class-less** target boxes, and serves a PPI previewer. Person/vehicle
labelling is **not** done here — that is the fusion module's job.

## I/O contract

**Consumes**
- CLI UART (default `/dev/radar-cli`, 115200) — profile push at startup.
- Data UART (default `/dev/radar-data`, **3,125,000** baud) — mmw_demo TLV
  point cloud (magic word + header + TLVs; DetectedPoints=1, SideInfo=7).

**Produces** — HTTP on **:8092** (mirrors the EO monitor shape):
- `GET /` → PPI previewer page; `GET /radar_view.js` → its script.
- `GET /stream` → **Server-Sent Events**, one JSON frame per radar frame.
- `GET /stats` → fps, drops, counts, connected, profile, max_range, plus the
  six live control values (`cluster_eps_m`, `cluster_min_pts`, `speed_min_mps`,
  `snr_min_db`, `fov_half_deg`, `doppler_gate_mps`).
- `GET /ctl?eps=&minpts=&speed=&snrmin=&fov=&doppler=` → set the host
  clustering live (`200 ok`). Six knobs; see
  [`docs/INTEGRATION.md`](docs/INTEGRATION.md) for ranges/defaults.

Frame JSON (stable — the GUI/fusion consume this unchanged):
```
{ connected, frame_id, timestamp, profile, max_range_m, fov_half_deg,
  num_points, num_targets,
  points:  [{x,y,z,v,snr,r,az,el,tid}, ...],
  targets: [{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,class}, ...] }
```
Sensor frame: `+x` right, `+y` forward (boresight), `+z` up, metres. `snr` is
the per-point SNR in dB (live; `null` only if a firmware without SideInfo is
flashed). `class` is always `radar_detection`.
`targets` are **per-frame detections only** — a box is emitted only for a
cluster seen this frame (**no coasting**); `tid` is stable across frames via
association. Display persistence and motion-model coasting are the GUI's and
the future tracking module's jobs, not the radar's.

## Build & run (on the Jetson, native aarch64)

```
cd radar/src && make            # pure C + pthreads + libm, no external deps
./radar_preview -w ../web       # push cfg/awr2944P_ag.cfg, stream, serve :8092
./radar_preview -s -w ../web    # SIMULATION: full pipeline, no board (see below)
```
Options: `-C` cli dev, `-D` data dev, `-c` cfg, `-b` data baud, `-p` port,
`-w` webroot, `-n` skip cfg push (chip already configured), `-s` simulate.

**Develop without hardware.** `-s` feeds a synthetic scene (walking person +
receding vehicle + static clutter) through the real parser/clusterer/wire path
and serves the same endpoints — so the previewer and the GUI can be built with
the Jetson off. The GUI integration contract is in
[`docs/INTEGRATION.md`](docs/INTEGRATION.md).

Standalone-runnable: with no board present it serves the page and reports
`connected:false`, retrying the ports — it never crashes on a missing peer.

## Where the compute runs

The **heavy DSP runs on the AWR2944P** — range/Doppler FFT, CFAR detection, and
angle-of-arrival are all in the mmw_demo firmware. The Jetson never does an FFT;
it receives the finished point cloud over UART. The host daemon only does
**drop-free parse + per-frame clustering (light association for stable ids +
velocity, no coasting) + serialize**, which is cheap:

| Host work per frame | Cost (Orin est., ~2–3× the x86 measurement) |
|---|---|
| Full path, typical ~100-pt frame | ~50 µs |
| DBSCAN + association, worst case 500 pts | ~1 ms |

That is **~1–2 % of one core at 30 Hz**, <1 MB RAM, and adds **<1 ms** latency —
the end-to-end latency floor is the chip's frame period, not this code. The only
superlinear cost is DBSCAN's O(N²); fine at mmw_demo point counts, swap to a
spatial-grid O(N) if counts ever spike. (Reproduce with the microbench under
`src/` — not shipped.)

**Migration to fully on-chip:** host-side clustering exists only because today's
firmware has no Group Tracker. The parser already handles the tracker TLVs
(308/309), so once Phase-2 firmware links `gtrack`, the chip emits target boxes
directly and this daemon drops to a pure parse-and-forward — no rewrite. (Full
tracking — motion-model coasting, long-term ids — is the downstream **tracking**
module regardless; the radar only ever emits per-frame detections.) See
[`docs/FIRMWARE.md`](docs/FIRMWARE.md).

## Layout
- `src/` — C daemon (`serial`, `cfg_push`, `tlv`, `cluster`, `wire`, `http`,
  `sim`). `-s` = simulation (no board); read-first startup (won't re-push to a
  live chip).
- `cfg/awr2944P_ag.cfg` — the shipped A/G long-range profile (LVDS off,
  per-point SNR on).
- `web/` — PPI previewer (`index.html`, `radar_view.js`) with a live tuning panel.
- `tools/radar_tlv_probe.py` — bench TLV dumper (diagnostic only).
- `docs/` —
  [`HARDWARE`](docs/HARDWARE.md) ·
  [`FIRMWARE`](docs/FIRMWARE.md) ·
  [`PREVIEW`](docs/PREVIEW.md) ·
  [`INTEGRATION`](docs/INTEGRATION.md) (GUI/fusion contract) ·
  [`TUNING`](docs/TUNING.md) (parameters + how to tune) ·
  [`ROADMAP`](docs/ROADMAP.md) (status + future work).
