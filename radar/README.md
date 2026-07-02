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
- `GET /stats` → `{fps, drops, num_points, num_targets, connected, profile,
  max_range_m, fov_half_deg}`.

Frame JSON (stable — the GUI/fusion consume this unchanged):
```
{ connected, frame_id, timestamp, profile, max_range_m, fov_half_deg,
  num_points, num_targets,
  points:  [{x,y,z,v,snr,r,az,el,tid}, ...],
  targets: [{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,coasting,class}, ...] }
```
Sensor frame: `+x` right, `+y` forward (boresight), `+z` up, metres. `snr` is
`null` when the firmware omits SideInfo. `class` is always `radar_detection`.

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

## Layout
- `src/` — C daemon (`serial`, `cfg_push`, `tlv`, `cluster`, `wire`, `http`).
- `cfg/awr2944P_ag.cfg` — the shipped A/G long-range profile (LVDS off).
- `web/` — PPI previewer (`index.html`, `radar_view.js`).
- `tools/radar_tlv_probe.py` — bench TLV dumper (diagnostic only).
- `docs/` — hardware, firmware, previewer detail.
