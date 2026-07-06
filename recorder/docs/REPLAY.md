# Replay engine + :8093 API

Replay serves a recorded session through the **same endpoint shapes the browser
already polls**, paced by a transport clock — the console renders a recording
with the exact code that renders live. Zero decode/encode: display JPEGs and
radar JSON come byte-verbatim off read-only mmaps. One session open at a time.

## Video source: NATIVE by default

Replay reconstructs the **full native 1440×1088** frame from the recorded
`eo_y10` channel so a session recorded with the display at a low res still
replays at full native detail. This is the default whenever the native channel
is present.

**Same tone map as the live feed — not a lookalike.** The decode→8-bit step
calls `eo_tonemap()` (`recorder/src/eo_tonemap.c`), the recorder's own copy of
the EO feed's tone map, kept byte-identical to `eo/pipeline/isp.c`. So a night
scene on replay is pixel-for-pixel what the operator could have seen live
(same adaptive p1/p99 stretch, temporal anti-breathing EMA, min-span floor,
gamma, and the median filter when it was on), just at full resolution. The EMA
advances frame-by-frame on forward play (matching live) and re-seeds on a
seek/jump.

### Keeping replay identical to the live feed — and knowing if it isn't

Three layers, so a divergence can never pass unnoticed:
1. **Automatic EO comparison (catches EO changing its process).** At replay
   open (~10 ms, once) the recorder compares one native frame — rendered through
   its own tone map — against the operator's recorded display JPEG, which came
   from the EO feed's *real* tone map, at a zoom=1 frame downscaled to match. If
   they diverge beyond JPEG noise, `/replay/state` reports
   `tonemap_vs_eo:"drift"` (with the mean pixel `tonemap_vs_eo_diff`) and
   `tonemap_match:false`. This is the direct check: if EO changes its tone map
   and this copy wasn't mirrored, the recorder catches it by itself. Verified:
   matched tone maps → `"ok"`, diff ~0.4; a changed tone map → `"drift"`, diff
   ~40. Reports `"unchecked"` only when a session has no zoom=1 display frame to
   compare against (e.g. radar-only, or always-zoomed).
2. **Device drift signature.** The recorder also stamps `tonemap_version` +
   `tonemap_hash` into `eo_y10/channel.json` at record time and re-checks at
   replay, catching the recorder's own tone map changing between record and
   replay (also folded into `tonemap_match`).
3. **Empirical bench check.** `tools/verify_replay_match.py` does the same
   native-vs-display comparison offline across many frames — run it in
   EO/recorder acceptance whenever the tone map is touched.

**Smooth full-quality native play over WiFi — cached H.264.** Native
JPEG-per-frame is ~12-24 MB/s, more than WiFi carries. So on replay open the
recorder transcodes the session's native replay into one H.264 MP4 (`transcode.c`
→ ffmpeg libx264, the exact tone map, `nice -n 15 ionice -c3` so it never touches
the live pipeline), cached in the session dir. The browser plays it with
`<video src=/replay/native.mp4?sid=…>` — buffered, smooth, full quality, instant
seek, and ~400× smaller on the wire than raw frames.
- `GET /replay/native.mp4?sid=<sid>` — the cached MP4 with HTTP Range (206) for
  `<video>` seeking; `202 {building,pct}` while it encodes.
- `/replay/state` reports `native_mp4` (`none|building|ready|failed`) +
  `native_mp4_pct`. Encoding is kicked on open, so it's usually ready by play.
- Still-frame paths (`/replay/frame`, pause/step) serve full-q native JPEGs
  directly — exact detail when inspecting.
- Fallbacks: the paced MJPEG `/replay/stream` (native emitted at `play_q`/
  `play_fps`, tunable via `/replay/ctl?playq=&playfps=`) covers the window while
  the MP4 builds; the DISPLAY source (small recorded JPEGs, verbatim) is the
  lightest option and needs no transcode.

- Falls back to the recorded display JPEGs (`eo_jpeg`, byte-verbatim) when the
  native channel is absent or has been dropped via `purge_native`.
- Toggle per open session: `GET /replay/ctl?video=native|display`.
- `/replay/state` reports `video_src`, `has_native`, `has_display`,
  `native_w/native_h` so the GUI can show and switch the source.
- Native frames are JPEG-encoded on demand at replay time (recorder runs at
  `Nice=5`, so it yields to the live pipeline), with a shared one-frame cache so
  concurrent viewers and pause/scrub don't re-encode. Zero cost when replaying
  the display source (bytes served verbatim).

## Transport semantics

- Clock: `t = t_ms + playing·(now−anchor)·rate`, clamped to [0, dur]; hitting
  the end auto-pauses. Every ctl re-anchors and wakes the pushers.
- `/replay/stream` (MJPEG): per-connection pusher emits the frame at the clock
  when it changes. High rates coalesce (skip to latest due frame, ≤60 fps out);
  pause freezes the `<img>` on the last frame; seek/step push exactly one frame
  down the same socket — scrubbing needs no reconnect.
- `/replay/radar` / `/replay/rstats` / `/replay/stats` answer polls with the
  recorded record at ≤ clock (≤120 ms latency, same class as live). Every
  replay JSON carries `"replay":true`.
- Mid-recording display-res changes: JPEGs are served verbatim; per-frame w/h
  rides in the record meta; recorded `eo_stats` events reproduce the status line.

## API (`:8093`, proxied by the console as `/rec/<path>`)

Recording / library:

| endpoint | effect |
|---|---|
| `GET /stats` | daemon health: state, rec_sid, rec_elapsed_s, disk_free_gb, est_min_remaining, per-channel {connected, records, bytes, mb_s, drops_ring, drops_queue}, pending_sid |
| `GET /ctl?rec=start` / `rec=stop` | start returns `{"ok":1,"sid":…}`; stop returns the sid to feed the save dialog |
| `GET /ctl?save=<sid>&name=&tags=a,b&note=` | pending→saved, generates thumbs (URL-encode name/note) |
| `GET /ctl?discard=<sid>` / `delete=<sid>[,…]` / `purge_native=<sid>` | discard pending / delete saved (bulk csv) / drop raw channel only |
| `GET /ctl?marker=<text>` | in-flight bookmark into the events channel |
| `GET /ctl?mode=y10p\|raw16\|y8` / `keep=N` | native-channel format / decimation (next session) |
| `GET /library?tags=a,b&q=text&state=saved` | summaries: sid,name,state,t0,dur_ms,tags,note,mode,thumbs,bytes{native,display,radar,meta} (tags AND-match, q case-insensitive on name+note) |
| `GET /session/<sid>` | full manifest |
| `GET /thumbs/<sid>/<n>.jpg` | preview stills n=0..7 (lazy-regenerated) |
| `GET /export?sids=<a,b,..>&tier=meta\|display\|full` | stream the selected sessions as one `.tar` download (meta = no video; display = + display JPEGs; full = + native raw). Sids validated; regenerable `native.mp4` never included |

Replay:

| endpoint | effect |
|---|---|
| `GET /replay/ctl?open=<sid>` / `close=1` | mmap/teardown (open replaces; refuses `state=recording`) |
| `GET /replay/ctl?play=1\|pause=1\|rate=<0.5..4>\|seek=<ms>\|step=1\|-1` | transport; step implies pause |
| `GET /replay/ctl?video=native\|display` | choose the video source (default native when present) |
| `GET /replay/stream` | paced MJPEG of the recorded display JPEGs |
| `GET /replay/radar` | recorded radar frame JSON at ≤ clock (schema of radar/docs/INTEGRATION.md + `"replay":true`) |
| `GET /replay/rstats` | recorded radar daemon /stats at ≤ clock |
| `GET /replay/stats` | `{replay:true, replay_state:{sid,name,t_ms,dur_ms,playing,rate,t_wall_ms,frame_i,frames}, eo:<recorded>, app:<recorded>}` |
| `GET /replay/state` | just `replay_state` — the 150 ms transport-bar poll |
| `GET /replay/frame?t=<ms>` | single JPEG at ≤ t (timeline hover preview) |
| `GET /replay/native.mp4?sid=<sid>` | cached H.264 of the native replay (HTTP Range/206 for `<video>` seek); `202 {building,pct}` while encoding |

> Pitfall: replay of the session currently being recorded is refused by design
> — stop first. Replaying any *other* session while recording is fine.
