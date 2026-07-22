# Radar module

TI **AWR2944PEVM** (77 GHz, 4TX/4RX), **no DCA1000**. The chip runs our
mmw_demoDDM firmware (**`agv3`** — crash-proof under point-flood overload, with
the empty-band comb gate in observe mode; see
[`docs/FIRMWARE.md`](docs/FIRMWARE.md)) and emits a TLV point cloud over
UART; this module pushes the profile, parses that stream with **zero frame
loss**, runs a **temporal multi-target tracker** plus a **slow
detector** for the faint/intermittent movers the per-frame tracker cannot
confirm, merges them into one **class-less** box per object and conditions the
published elevation (see [`docs/SLOWDET.md`](docs/SLOWDET.md)), and serves a PPI
previewer. Person/vehicle labelling is **not**
done here — that is the fusion module's job.

## I/O contract

**Consumes**
- CLI UART (default `/dev/radar-cli`, 115200) — profile push at startup.
- Data UART (default `/dev/radar-data`, **3,125,000** baud) — mmw_demo TLV
  point cloud (magic word + header + TLVs; DetectedPoints=1, SideInfo=7).

**Produces** — HTTP on **:8092** (mirrors the EO monitor shape):
- `GET /` → PPI previewer page; `GET /radar_view.js` → its script.
- `GET /stream` → **Server-Sent Events**, one JSON frame per radar frame.
- `GET /stats` → fps, drops, counts, connected, profile, max_range, plus the
  ten live control values (`cluster_eps_m`, `cluster_min_pts`, `speed_min_mps`,
  `snr_min_db`, `fov_half_deg`, `el_max_deg`, `doppler_gate_mps`, `confirm`,
  `coast_s`, `park_s`).
- `GET /ctl?eps=&minpts=&speed=&snrmin=&fov=&elmax=&doppler=&confirm=&coast=&park=` →
  set the live tracker knobs (`200 ok`; absent params keep their value). Meanings
  (see [`docs/TUNING.md`](docs/TUNING.md) for ranges/defaults):
  `eps`→dedup radius, `minpts`→seed points, `speed`→Doppler motion threshold,
  `snrmin`→point-strength gate, `fov`→azimuth gate, `elmax`→elevation half-angle
  gate (deg, radar-frame; 5–90, default 20 = the antenna's physical elevation
  beam edge, 90 = off), `doppler`→merge
  velocity-coherence, `confirm`→M-of-N confirm hits (latency vs false alarms),
  `coast`→seconds a track survives a dropout, `park`→seconds a stopped mover is
  held.

**Produces** — recorder taps (shared memory; protocol per
[`recorder/docs/TAP.md`](../recorder/docs/TAP.md) v1; vendored header
`tap/airpoc_tap.h`; never blocks the daemon, one `memcpy` per read/frame;
best-effort — logs once and runs unchanged if shm creation fails):
- `airpoc.radar_raw` — **512 × 8 KiB**, raw UART bytes verbatim *before* the
  parser (both read sites). No `meta`.
- `airpoc.radar_wire` — **16 × 256 KiB**, the `/stream` frame JSON byte-verbatim.
  `meta[6] = {frameNumber, n_points, n_targets, 0, 0, 0}`.
- `airpoc.radar_cli` — **64 × 8 KiB**, the chip's own CLI telemetry: the plain-text
  reply to `queryDemoStatus`, polled once per second. No `meta`. Carries the
  empty-band comb-gate margin histogram, sensor state, UART deferred-frame count,
  and RF calibration status + chip temperature.

  > Pitfall: this telemetry exists **only** on the CLI UART — it is not in the TLV
  > point stream. A recording made without this tap cannot answer any question
  > about comb margins afterwards; it needs someone live at the bench. That is why
  > it is recorded rather than merely displayed.

  Read it back out of a recording with
  [`tools/comb_margin.py`](tools/comb_margin.py) (`--from/--to` split one movie
  into its static and moving parts).

  > Pitfall: **start the recording within ~1 minute of START.** Measured
  > 2026-07-18: every `/dev/shm/airpoc.*` tap — not just this one — is unlinked
  > roughly 100–150 s after the stack starts, while the producers stay alive and
  > healthy. After that the recorder captures almost nothing (radar_raw stops at
  > ~12 KB) even though the live view still works. A recording begun promptly
  > captures normally for its duration: verified 57 telemetry samples, 13 MB
  > radar_raw and 9.9 GB eo_y10 over 90 s. This is the known tap gremlin in the
  > launcher/tap area (see `app/docs/GUI.md`), not a radar-module fault.

Frame JSON (stable — the GUI/fusion consume this unchanged):
```
{ connected, frame_id, timestamp, profile, max_range_m, fov_half_deg,
  num_points, num_targets,
  points:  [{r,az,el,v,snr,tid}, ...],        // polar canonical; r is SLANT range:
                                              // x=r*cos(el)*sin(az), y=r*cos(el)*cos(az),
                                              // z=r*sin(el)
  targets: [{tid,x,y,z,vx,vy,vz,sx,sy,sz,conf,np,sus,mv_class,class}, ...] }
```
Sensor frame: `+x` right, `+y` forward (boresight), `+z` up, metres (`z` carries
elevation). `snr` is the per-point SNR in dB (live; `null` only if a firmware
without SideInfo is flashed). `class` is always `radar_detection`. `tid` is a
stable per-target id.

`sus` (0/1, added 2026-07-16): **suspected antenna-sidelobe copy.** The target
currently sits at the same range with the same signed speed as a stronger
emitted target, well separated in azimuth — the signature of a reflection.
It is still published (a genuine second target crossing another's range looks
the same for a second or two), but consumers that act on targets (gimbal cueing,
fusion) should treat a `sus:1` box with caution until the flag clears. Copies
that hold this signature continuously for several seconds stop being published
altogether.

`mv_class` (0/1/2, added 2026-07-16): **motion verification.** `1` = verified
mover — the target's claimed doppler adds up to the distance it actually
walked (or it shows real cross-range motion); the strongest "this is real"
signal the radar can give. `0` = unverified-slow — radially quiet or too
faint to judge (crossers, parked targets); still perfectly valid, just not
provable this window. `2` = suspect — its claimed motion is currently
contradicted by its own position history; targets that stay in this state
stop being published. A verified target that slows down drops back to `0`,
it does not die.

Target `x,y,z` / `vx,vy` are the tracker's **guidance output filter** state
(alpha-beta smoothed angles + doppler-aided range-rate, slew-limited on
re-acquire so a coasted track never teleports) — the wire is what a gimbal
should steer on. Raw per-frame cluster medians stay internal; association and
lifecycle are unchanged by the filter. Points (`points[]`) are raw as before.

> Note: azimuth/elevation are the radar's own frame — the radar↔EO calibration
> offsets are applied by the GUI/fusion, not baked into the wire.

`targets` are **confirmed temporal tracks**: a target is emitted after it passes
an M-of-N confirmation (fast for Doppler-backed movers, slower + consistency for
static-born ones), and it **coasts briefly** through short dropouts; a track
that moved then stopped is held (parked-car persistence). Ids are stable across
frames. Longer-horizon fusion (person/vehicle labelling, cross-sensor) remains
downstream.

### Two detectors, one box per object

`targets` come from **two** detectors, merged by `fuse.c` into a single list:

- **cluster** (`cluster.c`) decides per frame and confirms over a few frames. It
  owns anything close or bright, and it is **authoritative** — fuse copies its
  targets through untouched and can only ever *add* to them, never remove.
- **slowdet** (`slowdet.c`) chains faint, intermittent echoes across frames. It
  holds what cluster cannot confirm: a 300 m car returning a single point in only
  ~60 % of frames, the night walker past 240 m. Its ids start at **1000**, so the
  source of any box is visible on the wire. See
  [`docs/SLOWDET.md`](docs/SLOWDET.md).

A slowdet target is emitted only where no cluster target already sits at the same
range and bearing, so an object is never boxed twice.

### Elevation is conditioned; range and azimuth are not

The array is spread sideways but barely stacked vertically, so **azimuth is
accurate (~0.6°) and elevation is not (~3.5°, 11° at the tail)**. Range and
azimuth are published at full frame rate, untouched. The elevation of **every**
published target — both detectors — passes through a trailing median whose window
scales with range (0.6 s at 200 m, clamped 0.3–1.2 s), a 20 °/s physical rate
limit, and a 1.5° cap on the vertical box half-height. Measured effect on the
published elevation: frame-to-frame jump **4.06° → 0.17°**.

This removes **jitter, not bias** — a repeatable offset is calibration's job. The
window is range-scaled rather than simply long because this is an aerial seeker:
a target's elevation angle changes at roughly (vertical speed ÷ range), so a
multi-second average would smear a real manoeuvring target worst in the terminal
phase. Rationale and the measurements behind every constant are in
[`docs/SLOWDET.md`](docs/SLOWDET.md); the approaches that failed are in
[`docs/TRIAL_AND_ERROR.md`](docs/TRIAL_AND_ERROR.md).

## Build & run (on the Jetson, native aarch64)

```
cd radar/src && make            # pure C + pthreads + libm + librt, no external deps
```

**With the udev rule installed** (recommended — see below), the defaults just work
from anywhere; cfg/web are resolved relative to the binary, not the CWD:
```
./src/radar_preview             # /dev/radar-cli + /dev/radar-data, ../cfg, ../web
```

**Without the udev rule**, name the raw XDS110 nodes explicitly (`ttyACM0` = CLI
interface 00, `ttyACM1` = data interface 03):
```
./src/radar_preview -C /dev/ttyACM0 -D /dev/ttyACM1
```
Either way the daemon **waits/retries** for the ports and the CLI console — the
board enumerates a few seconds after boot, so a cold start is fine.

Install the stable device names once (survives ACM renumbering):
```
sudo cp udev/70-radar.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```

Options: `-C` cli dev, `-D` data dev, `-c` cfg (default: `../cfg/awr2944P_ag.cfg`
relative to the binary), `-b` data baud, `-p` port, `-w` webroot (default:
`../web` relative to the binary), `-n` skip cfg push, `-s` simulate.

**Develop without hardware.** `-s` feeds a synthetic scene (walking person +
receding vehicle + static clutter) through the real parser/tracker/wire path
and serves the same endpoints — so the previewer and the GUI can be built with
the Jetson off. The GUI integration contract is in
[`docs/INTEGRATION.md`](docs/INTEGRATION.md).

Standalone-runnable: with no board present it serves the page and reports
`connected:false`, retrying the ports — it never crashes on a missing peer.

## Where the compute runs

The **heavy DSP runs on the AWR2944P** — range/Doppler FFT, CFAR detection, and
angle-of-arrival are all in the mmw_demo firmware. The Jetson never does an FFT;
it receives the finished point cloud over UART. The host daemon only does
**drop-free parse + temporal tracking + serialize**, which is cheap:

| Host work per frame | Cost (Orin est., ~2–3× the x86 measurement) |
|---|---|
| Full path, typical ~250-pt frame | ~0.4 ms |

That is **~1 % of one core at 26–30 Hz**, <1 MB RAM, and adds well under a
millisecond of latency — the end-to-end floor is the chip's frame period, not
this code. Everything is O(points × tracks) with fixed buffers and no heap on
the hot path; point counts are a few hundred, track counts a few tens.

**Migration to fully on-chip:** host-side tracking exists only because today's
firmware has no Group Tracker. The parser already handles the tracker TLVs
(308/309), so once Phase-2 firmware links `gtrack`, the chip emits target boxes
directly and this daemon drops to a pure parse-and-forward — no rewrite, because
the tracker sits behind the same `cluster_step` interface. See
[`docs/FIRMWARE.md`](docs/FIRMWARE.md).

## Versions

The radar ships as locked, fully-packaged versions — each folder holds the fw
image + flash cfg + exact fw sources, the chip cfg, and the tracker sources of
that version, so any layer can be reverted independently.

- [`v1/`](v1/README.md) — **V1, frozen 2026-07-10** (tag `AIRPOC-RADAR-V1.0`;
  revert recipe: [`VERSION_V1.0.md`](VERSION_V1.0.md)).
- [`v2/`](v2/README.md) — **V2, shipped 2026-07-11** (crash-proof fw `agv2` +
  consistency-guard tracker; on-chip now).
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — what each version is, what's open,
  what's next.

## Layout
- `src/` — C daemon (`serial`, `cfg_push`, `tlv`, `cluster`, `wire`, `http`,
  `sim`). `cluster` is the temporal multi-target tracker (class-less). `-s` =
  simulation (no board); read-first startup (won't re-push to a live chip).
- `cfg/awr2944P_ag.cfg` — the shipped A/G long-range profile (V2: 16.0 dB
  doppler CFAR, BFP compression 0.75, rangeProfile TLV off, LVDS off,
  per-point SNR on). The `awr2944P_ag_v2_*.cfg` files are the staged V2
  cfg-wave variants kept for reference/rollback.
- `web/` — PPI previewer (`index.html`, `radar_view.js`) with a live tuning panel.
- `tools/` — bench/offline tools (Python + one C harness): `radar_tlv_probe.py`
  (TLV dumper), `radar_tracker.py` (tracker reference) + `track_replay.c` +
  `parity_check.py` (C-vs-reference replay validation), `walkout_score.py`
  (max-range walk scorer), `regression/` (fixture conversion + baseline
  fingerprints — see [`docs/TEST_CORPUS.md`](docs/TEST_CORPUS.md)).
  `track_replay` prints one `K` header (the effective knob state it ran with)
  then `F`/`C`/`E` lines per frame; `E` lines are the wire (tid, r, az, vr,
  snr_peak) and are the ONLY lines validation scores. Every tracker change
  must pass `regression/tracker_gates.py` before deploy — see
  [`docs/VALIDATION.md`](docs/VALIDATION.md).
- `v1/`, `v2/` — the locked version packages (above).
- `docs/` —
  [`HARDWARE`](docs/HARDWARE.md) ·
  [`FIRMWARE`](docs/FIRMWARE.md) ·
  [`FRAMERATE`](docs/FRAMERATE.md) (how high we can go + cost) ·
  [`PREVIEW`](docs/PREVIEW.md) ·
  [`INTEGRATION`](docs/INTEGRATION.md) (GUI/fusion contract) ·
  [`TUNING`](docs/TUNING.md) (parameters + how to tune) ·
  [`ROADMAP`](docs/ROADMAP.md) (versions + status + future work) ·
  [`TEST_CORPUS`](docs/TEST_CORPUS.md) (living catalog of test recordings) ·
  [`VALIDATION`](docs/VALIDATION.md) (the tracker bench + deploy checklist) ·
  [`SHIP_RUNBOOK_V2`](docs/SHIP_RUNBOOK_V2.md) (V2 ship record + the open
  comb-gate/bar-ladder steps) ·
  [`AG_FW_PLAN`](docs/AG_FW_PLAN.md) (historical fw root-cause analysis) ·
  [`PHASE3_ANGLE_MOTION_SPEC`](docs/PHASE3_ANGLE_MOTION_SPEC.md) (crossing-traffic
  detector spec).
