# AIREC v1 — on-disk session format (frozen)

Why not MCAP: its writer is C++/STL, its index lands at end-of-file (power loss
= recovery tooling on every flight), and our dominant payload (packed mono-10)
has no standard encoding a generic viewer renders anyway. AIREC is append-only,
crash-safe by construction, ~300 lines of dependency-free C. An offline
`airec2mcap` converter is the stated path if Foxglove viewing is ever wanted.

## Session directory

```
/data/recordings/<sid>/            sid = UTC "20260704T142233Z", IMMUTABLE
  manifest.json                    atomic write -> fsync -> rename
  <channel>/channel.json           self-description (encoding, meta schema)
  <channel>/data.NNNNN.airec       segments (heavy: fallocate 256 MiB, O_DIRECT)
  <channel>/index.bin              append-only 32 B rows
  thumbs/0..7.jpg                  written at save
  native.mp4                       cached H.264 of native replay (see REPLAY.md)
  native.mp4.ver                   encoder version stamp (bump => rebuild)
```

Channels today: `eo_y10` `eo_jpeg` `radar_raw` `radar_wire` `det_wire` `events` `radar_cli` `trk_wire` `fus_wire`.
(ChanId is append-only: the numeric id is stamped into every segment header on
disk, so a new channel goes at the END of the enum, never in the middle.)
New channels = new directory, same three files; no format change. `eo_y10`'s
`channel.json` also carries `w`/`h`, `tonemap_version`, and `tonemap_hash`
(the device tone-map signature — see REPLAY.md drift check).

## Binary layout (little-endian, all structs 64 B / 32 B)

Segment header (offset 0 of every data file):
`u64 magic "AIRECSG1" · u32 version=1 · u32 channel_id · u64 session_t0_mono_ns · u32 segment_no · pad[36]`

Record: header + payload + zero-pad to 8:
`u32 magic "REC0" · u32 crc32c(payload) · u64 seq · u64 t_src_ns · u64 t_pub_ns · u32 payload_len · u32 flags · u32 meta[6]`

Flags: `1` TRUNCATED · `2` GAP_BEFORE (drops precede this record) · `4` PAD
(alignment pseudo-record — skip; the index never points at one).

Index row: `u64 seq · u64 t_ns · u32 segment_no · u32 offset · u32 payload_len · u32 flags`
— `t_ns` is the record's `t_pub` (recorder CLOCK_MONOTONIC, common to every
channel — so the replay timeline aligns across channels even though `eo_y10`'s
source `t_src_ns` is the camera's own V4L2 clock). Binary-search by `t_ns`;
rebuildable by scanning segments.

> Segments are **unbounded** — heavy channels roll a new 256 MiB `data.NNNNN`
> every ~2 s of native, so a long recording has hundreds (a 3:37 session ≈ 96).
> Any reader (replay, transcode) MUST cover every segment the index references,
> not a fixed cap, or long recordings truncate on playback (the recording itself
> is always complete).

## Per-channel meta[6] and payloads

| channel | payload | meta[6] |
|---|---|---|
| eo_y10 | packed 10-bit bitstream (`y10p`: 4 px → 5 B, p0 bits 0–9…), or `raw16`/`y8` per `channel.json.encoding` | v4l2_sequence, exp_lines, gain, vmax, mean10_x100 **or illum_packed**, drops_cum |

> When the EO tap declares `"illum":1` in its `meta_json`, `eo_y10` meta[4] is the
> per-frame illuminator instead of `mean10_x100`, packed:
> `bit0 on · bit1 present · bits8-15 power(0-255) · bits16-25 FOV×10 (0.0-102.3°)`.
> `channel.json` records `"illum":1` and names the slot `illum_packed`; replay
> exposes it as `illum:{on,power,fov,present}` in `/replay/state`. Backward
> compatible: without the flag, meta[4] stays `mean10_x100` and nothing changes.
| eo_jpeg | display JPEG **byte-verbatim** as served to the operator | eo_seq, dw, dh, zoom, res_idx, 0 |
| radar_raw | UART bytes exactly as `read()` returned (re-feedable to the TLV parser) | read_len |
| radar_wire | the exact SSE frame JSON | frame_number, n_points, n_targets |
| det_wire | the EO-detector frame JSON (verbatim) | frame_id, n_dets, n_movers |
| trk_wire | EO tracker wire (trackerd :8095), verbatim SSE JSON | frame_id, n_tracks |
| fus_wire | radar+EO fusion wire (fusiond :8096), verbatim SSE JSON | frame_id, n_fused, n_eo_only, n_rad_only |
| radar_cli | radar chip CLI telemetry: `queryDemoStatus` replies, raw ASCII, ~1 Hz. Comb-gate margin histogram, sensor state, UART deferred-frame count, RF cal status, chip temp. Exists only on the CLI UART — unrecorded means lost | (none) |
| events | JSON `{"type":"eo_stats\|radar_stats\|app_stats\|clock_anchor\|marker\|channel_lost\|channel_resumed","t_mono_ns":…,"body":{…}}` | — |

> `channel_lost`/`channel_resumed` are the loss watchdog (below): if a tap-fed
> channel stops producing >2 s mid-recording, `channel_lost` (with the
> last-frame timestamp) is recorded here and `/stats` sets that channel's
> `lost:1`, so a dropped feed is never silent. `channel_resumed` when it recovers.

## Clocks

Every `t_src_ns`/`t_pub_ns` is CLOCK_MONOTONIC. Wall time via `clock_anchor`
events ({mono_ns, realtime_ns} at start + every 30 s) and `manifest.t_start`.
eo_y10 `t_src_ns` is the kernel V4L2 buffer timestamp (exposure-referenced).

## Manifest

Mutable prefix (state/name/tags/note/ai) then immutable tail from `"t_start"` —
edits rewrite the prefix, preserve the tail. States:
`recording → pending → saved | discarded`; crash → tails truncated at last
CRC-valid record → `recovered` (treated as pending). `ai{}` is reserved for the
v2 auto-annotation pipeline.

> Pitfall: fallocate'd segment tails are zeros; recovery must CRC the last
> record's payload, not just find its header (zeros carry valid-looking pads).
> `session.c:recover_channel` and `airec_dump.py --verify` both do.
