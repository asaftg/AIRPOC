# Replay engine + :8093 API

Replay serves a recorded session through the **same endpoint shapes the browser
already polls**, paced by a transport clock — the console renders a recording
with the exact code that renders live. Zero decode/encode: display JPEGs and
radar JSON come byte-verbatim off read-only mmaps. One session open at a time.

## Video source: NATIVE by default

Replay reconstructs the **full native 1440×1088** frame from the recorded
`eo_y10` channel — decode packed 10-bit → per-frame percentile tonemap + gamma
→ JPEG (`render.c`) — so a session recorded with the display at a low res
still replays at full native detail. This is the default whenever the native
channel is present.

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

> Pitfall: replay of the session currently being recorded is refused by design
> — stop first. Replaying any *other* session while recording is fine.
