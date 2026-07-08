# GUI integration — work order for the console (app/) owner

The recorder daemon (`:8093`) is running and serves everything below today.
Endpoint schemas: [REPLAY.md](REPLAY.md). Nothing in eo/, radar/, or the
recorder needs the console for correctness — this work order is pure UI.

## 1. C side — ONE change: `/rec/` pass-through in gui.c

Route `GET /rec/<anything>` by opening a TCP connection to the recorder
(default `127.0.0.1:8093`, new `main.c` option `-c` like `-e`/`-r`), sending
`GET /<anything> HTTP/1.0\r\nHost: rec\r\n\r\n`, then **splicing bytes to the
client until EOF** (read→write loop, no buffering of whole responses — this
relays the replay MJPEG stream). On connect failure: `502` with
`{"connected":false}`. `handle_ctl` untouched — recorder control flows as
`/rec/ctl?...` through the pass-through. No new client .c file needed.

## 2. REC button (`app.js:150` — currently a cosmetic class toggle)

- Poll `/rec/stats` every 400 ms (alongside `pollRstats`). Fetch fail or
  `connected:false` → dim the button, tooltip "RECORDER NOT CONNECTED".
- Idle → click → `GET /rec/ctl?rec=start` → `.active` (existing red style) +
  blinking dot + elapsed `REC 00:34` from `rec_elapsed_s`.
- Recording → click → `GET /rec/ctl?rec=stop` → response `{"sid":…}` → open
  the save dialog for that sid.
- If `/rec/stats` shows `pending_sid` at page load (stop happened while the
  page was away), offer the save dialog for it.
- **Feed-loss warning while recording:** each `/rec/stats` channel has `lost`.
  If any is `1` during a recording, show a loud "⚠ FEED LOST — <channel>"
  banner (a live sensor feed stopped mid-recording). It clears when the feed
  resumes (`lost:0`).

## 3. Save dialog (`#recdlg`, modal; live feeds keep running behind it)

- Name input, prefilled `REC <local datetime>`.
- Tag chips (toggle), vocabulary v1: `night day human vehicle drone long-range
  short-range radar tracking fusion illum test bug demo calibration`.
- Free-text note.
- **SAVE** → `GET /rec/ctl?save=<sid>&name=<urlenc>&tags=<a,b>&note=<urlenc>`.
- **DISCARD** → confirm → `GET /rec/ctl?discard=<sid>`.
- Dismissal without either leaves the session pending (finishable from the
  library; auto-purges after 24 h). AI-annotate button: v2 — leave space.

## 4. LIBRARY tab (topbar button next to DEV)

- Grid of cards from `GET /rec/library`: poster `/rec/thumbs/<sid>/2.jpg`,
  hover cycles `0..7.jpg` at ~6 fps (timer swapping `src`). Card: name, local
  date from `t0`, `MM:SS` from `dur_ms`, size badge (display+meta bold,
  `+ raw NN GB` dimmed when `bytes.native > 0`), tag chips, PENDING ribbon
  when `state != "saved"`.
- Filter bar: tag multiselect + text box → refetch `/rec/library?tags=&q=`
  (or client-side filter; both work).
- Selection mode: checkboxes → `DELETE (n)` (confirm →
  `/rec/ctl?delete=a,b,c`), per-card "FREE SPACE — drop raw" →
  `/rec/ctl?purge_native=<sid>` (confirm).
- **OFFLOAD (n)** — download the selected sessions: a tier picker (meta =
  annotations+thumbs+radar · display = + the video · full = + native raw) then
  `window.location = "/rec/export?sids=a,b,c&tier=display"`. The recorder streams
  one `.tar`; the browser saves it — no progress endpoint, it's a normal
  download. (For very large `full` pulls, the workstation
  `offload_pull.sh`/`airpoc-offload.ps1` scripts stay the resumable option.)
- Disk bar from `/rec/stats` `disk_free_gb`/`disk_total_gb` +
  `est_min_remaining`.

## 5. Replay view

- Card click → `GET /rec/replay/ctl?open=<sid>` → replay mode **reusing the
  live DOM/renderers**: introduce
  `var API = {stream:"/stream", radar:"/radar", stats:"/stats", rstats:"/rstats"}`,
  point the pollers at `API.*`; replay sets them to `/rec/replay/*` and
  `#video.src = "/rec/replay/stream"`. `drawRadar`/target list/EO status then
  render the recorded data unchanged. Note: `/rec/replay/stats` nests the eo
  object as `.eo` like the live `/stats`, and adds `.replay_state`.
- **Unmistakably replay**: striped amber banner `REPLAY — <name> — <t0>Z`,
  ZULU pill shows recorded wall clock (`replay_state.t_wall_ms`) labeled REC,
  `body.replay` hides/disables ALL live controls (LIGHT/ILLUM/TRACK/REC,
  zoombar, DEV inputs — show recorded values read-only; send no `/ctl`).
- **Video source — NATIVE by default:** replay reconstructs the full native
  1440×1088 frame from the raw channel, not the recorded display res, so low-res
  display recordings still replay at full detail. `/rec/replay/state` exposes
  `video_src` ("native"/"display"), `has_native`, `has_display`, `native_w/h`.
  Add a `NATIVE / DISPLAY` toggle in the transport → `/rec/replay/ctl?video=native|display`
  (hide when `has_native:false`, e.g. radar-only or purged-raw). The `<img>`
  renders whatever res arrives; CSS `object-fit` keeps the layout stable on switch.
  - **Smooth native play over WiFi (recommended path):** for NATIVE playback use
    a `<video src="/rec/replay/native.mp4?sid=<sid>">` element, not the MJPEG
    `<img>`. The recorder transcodes the session to a cached H.264 on open;
    `/rec/replay/state` reports `native_mp4` (`none|building|ready|failed`) +
    `native_mp4_pct`. While `building`, show "preparing native… N%" (and you may
    play the paced `/rec/replay/stream` MJPEG meanwhile); when `ready`, point the
    `<video>` at the MP4 — buffered, smooth, full quality, instant seek. The
    `<video>`'s own timeline/seek can drive the transport, or keep your transport
    bar and map it to the video element. Range requests are supported (206).
    Use the MJPEG `<img>` stream for the DISPLAY source and for radar-only sessions.
  - **Tone-map integrity:** `/rec/replay/state` has `tonemap_match` (bool) and
    `tonemap_vs_eo` (`ok`|`drift`|`unchecked`). Normally `ok`/true — native
    replay renders with the exact live tone map. If `tonemap_vs_eo:"drift"` (the
    recorder auto-detected its tone map diverging from the EO feed), show a small
    caveat badge ("tone map differs from live feed") instead of presenting it as
    exact. `unchecked` just means no comparable frame existed — no badge needed.
- **Detection boxes in replay:** poll `/rec/replay/det` exactly like the live
  detector feed — it returns the recorded EO-detector frame JSON at ≤ the
  playback clock (same schema: `dets[]` with `cls`/`conf`/`px`/`ang`, plus
  `"replay":true`), so point your existing detection overlay at it and the boxes
  draw with no other change. `px` is native 1440×1088 pixels; apply the same
  zoom/letterbox mapping you use live. 404 = the session has no detections
  (hide the overlay). Detections follow the same seek/scrub/pause clock as video.
- Transport bar under the video: play/pause → `/rec/replay/ctl?play=1|pause=1`;
  rate cycle 0.5/1/2/4× → `rate=`; frame step ⏮⏭ → `step=-1|1`; range-input
  timeline (`max=dur_ms`, `oninput` throttled ≥80 ms → `seek=<ms>`, suppress
  playhead writeback while dragging); readout `mm:ss.s / mm:ss.s`; optional
  hover preview `<img src="/rec/replay/frame?t=…">`. Poll `/rec/replay/state`
  at 150 ms for the playhead (`playing:false` at end = show ⏵ replay-again).
- Keyboard: `←/→` step (auto-pauses), `Space` play/pause, `↑/↓` rate,
  `Esc` → `/rec/replay/ctl?close=1` → back to library.
- Any `/rec/*` failure in replay → NOT CONNECTED scrim — never a frozen frame
  passing as live.

> Pitfall: the pass-through must stream, not buffer — `/rec/replay/stream`
> never ends. Reuse the eo_client.c connect/relay pattern.
