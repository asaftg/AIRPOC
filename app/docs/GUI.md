# Operator console — layout, controls, endpoints

The console is one self-contained web page (embedded in the binary) served by a **thin
proxy**. The app consumes the EO module's video feed and the radar daemon's frames,
forwards operator controls to each, and adds the integrated picture + the record/replay
UI. **No websockets, no framework, no CDN** (the operator is on the drone link with no
internet — text labels + inline SVG + canvas + SSE only). **No capture / ISP / AE /
encode / illuminator serial in the app** — each module owns its domain.

Turning the whole stack on/off and the field networking is a separate always-on service —
see [`launcher/README.md`](../launcher/README.md) (`:8088` control page: START/STOP,
AP/WIFI/AUTO, SHUTDOWN, honest per-feed status).

## Layout
Full-viewport, no scroll. **EO fills the left ~70%**; the right column stacks a **target
list** over the **radar scope** (forward half-circle sector, not a 360° PPI). Controls
live *inside* the EO panel so they never cover the radar origin. On a **phone (≤720px)**
it stacks vertically — EO on top, radar under it, targets at the bottom; DEV + LIBRARY go
full-screen (see `app.css` media query).

- **Top bar:** `FAZE-01`, link chip (signal bars · type USB/WIFI · live Mb/s · delivered
  fps), BATT/ALT (reserved), ZULU (client UTC), NIGHT (day/night theme), **ROI**,
  LIBRARY, DEV.
- **EO (left):** proxied video, amber cross reticle, FOV/zoom + BRG/RNG lines, the
  **radar→EO overlay** (all radar targets projected onto the video — see below), detector
  boxes, zoom **±** (bottom-left), the control cluster (bottom-centre). Scrim shows
  **EO · NOT CONNECTED** / **NO VIDEO RECORDED** (replay of a radar-only session).
- **Target list (right, top):** **both sensors** — EO tracks (class · confidence · az/el) and
  radar targets (range · speed), source-tagged, no dedup until fusion exists. Tap a row to
  select it — see *Target list* and *Tracking* below.
- **Radar scope (right, bottom):** see *Radar rendering* below. Expand button flips it to
  the big view (EO drops to a PIP).
- **Control cluster (over EO):** `LIGHT` (fire) · `TRACK` (auto/man) · `REC` (record).
- **DEV drawer:** a right-edge overlay (same in EO-big and radar-big) — STREAM, EO SENSOR,
  ILLUMINATOR, RADAR knobs, SYSTEM.

## Live vs replay
The same renderers show live feeds or a recording — an `API` base object swaps
`{stream,radar,stats,rstats}` to `/rec/replay/*` in replay. `body.replay` disables the
live controls (but keeps DEV ✕ / RETURN TO CONTROL usable). A striped amber banner + a
transport bar (play/pause/step/rate/scrub/hover, NATIVE↔DISPLAY when a native channel was
recorded) drive playback. Un-recorded channels show **NO VIDEO / NO RADAR RECORDED**.

- **EO video source in replay.** A replay **opens on the DISPLAY channel** (instant): the
  paced MJPEG `/rec/replay/stream` (the recorder caps it ~20 fps for the link). **NATIVE** =
  the recorded full-res **60 fps** H.264 `/rec/replay/native.mp4` in a `<video>`. Readiness is
  read from `/rec/replay/state` → `native_mp4` (`none|building|ready|failed`) + `native_mp4_pct`
  — the console does **not** probe the endpoint. **Auto-native:** the moment the recorder
  reports the HD mp4 `ready` (already built / cached), the console switches to it once and
  streams it — no rebuild, no tapping NATIVE. A not-yet-built movie **stays on DISPLAY and
  never auto-builds**; the operator triggers a build explicitly (the card's **Convert to HD**
  button, or the NATIVE toggle). While building, a **"PREPARING NATIVE 60 fps · N%"** badge
  shows over the paced MJPEG. The `<video>` plays on its **own decode clock**, corrected to the
  transport clock only on a **big (>0.3 s) drift**; scrubbing seeks it directly per drag.
  (Hard-setting `currentTime` every poll — the old behaviour — turned playback/scrub into
  constant seeking that reset to frame 0.) If the mp4 can't decode it falls back to MJPEG.
- **Replay radar + detections push over SSE** — `/rec/replay/radar/stream` and
  `/rec/replay/det/stream`, each paced to the playback clock (honours pause/seek/rate) so the
  scope (~26 Hz) and boxes track smoothly. `closeReplay` closes both on EXIT (not on next-open).
- **Connection budget.** A browser allows only ~6 HTTP/1.1 sockets per host, so the console
  minimises long-lived streams: the **library list holds ZERO** (video/radar/det are closed on
  library-open), and a **replay holds few** (the mp4 `<video>` + the two overlay SSE, all closed
  on EXIT). This is why "can't open a recording after 3-4 cycles" and the FIN-WAIT socket pileup
  are gone. An in-flight open is generation-guarded (`replayGen`) so exiting to live mid-open
  can't flip `replaying=true` over the live view (which used to dead-lock REC + LIGHT).
- **Self-heal.** If the recorder has no open session (it restarted, or a reboot) but the
  console still thinks it's replaying, `pollReplayState` detects `open:false` (after a few
  consecutive polls, once the session was actually seen open) and resets to live automatically.
- **Live sensors stay UP during review** — they are **not** stopped while browsing/replaying.
  (An earlier "suspend the producers in the library" feature was **removed**: stopping and
  restarting eo_pipeline/radar/detector thrashed the box — the camera re-init + detector engine
  reload pegged the CPU and wedged the camera after a few enter/exit cycles, unrecoverable
  without a reboot. The recorder no longer transcodes-on-open, so keeping the stack live costs
  nothing.) A live-view backstop still re-runs the launcher `/resume` if EO is genuinely down
  (a crashed producer) for ~2.5 s.

## Zoom + ROI
- **EO zoom ±** — live: forwards `zoom=` to the EO feed (source crop). Replay: a client-side
  CSS zoom on the recorded frame, applied to the shown video (MJPEG `<img>` or native
  `<video>`) **and the overlay canvas together** so the boxes track (click to recenter). The
  zoom readout in **DISPLAY** replay shows the zoom the frame was **captured** at (from the
  recorded stats) × any client digital zoom — a clip shot at 2× reads "2.0×". **NATIVE** is
  the full frame, so its readout is pure client digital zoom.
- **ROI** (top bar) — press to arm, drag a box on the **EO or radar**, it zooms into that
  region; press again to reset. Live EO = CSS scale to the box; in **replay** the EO ROI sets
  the digital-zoom level on all layers (video + overlay); radar = a pan+zoom world window.
  Works live and in replay; resets on replay enter/leave. Purely client-side rendering.

## Radar rendering (console-owned)
- **Push, not poll.** Live radar arrives over **SSE `/radar/stream`** at the sensor's
  native rate (~27 Hz). Replay radar also pushes over **SSE `/rec/replay/radar/stream`**
  (paced to the playback clock), with a `/rec/replay/radar` poll fallback. Frames are the
  daemon's schema, drawn verbatim.
- **FOV clip.** Only points/targets whose azimuth is within **±FOV** (`fov_half_deg`) are
  drawn — tracks the FOV knob live and the recorded value in replay.
- **Range / zoom.** `AUTO · 50 · 100 · 250 · 500 m` selector (top-left of the scope). AUTO
  = adaptive stretch (grows to fit the farthest target); the presets pin the range. Grey
  metric range-grid rings + **constant amber reference rings at 100 m and 250 m** on every
  zoom.
- Doppler colours (red inbound / blue outbound / static), per-point SNR alpha, dashed FOV
  wedge + boresight. A track wears ONE colour everywhere (scope, list, EO); the **engaged**
  target is the exception — it takes green LOCK, since the tracking phase is live now and the
  held target must be unmistakable. **No GUI-side persistence** — the daemon is
  a temporal tracker (stable tids, M-of-N confirm, coast, park-hold), so target boxes and
  list rows are drawn verbatim from the frame; a GUI hold would double-persist.

## Radar → EO overlay (console-owned render; NOT fusion)
- Every radar target is projected onto the video from its az/el (radar frame) through the
  camera's current hfov/vfov, drawn as a **broken halo ring** (four arcs, dark halo underlay) in its track
  colour, labelled `R#tid range` (size-coding by the tracker's sx/sy was tried and pulsed —
  those estimates jitter; position is stable). Off-frame targets are not drawn. (These are the
  RADAR's marks on the video — distinct from the EO tracker's boxes above, and not fused with
  them; a radar ring and an EO box on the same object are two sensors, not one target.)
- **DEV → RADAR ON EO**: OVERLAY on/off + **AZ TRIM / EL TRIM** (±10°, 0.1° steps) + **SAVE** —
  the radar↔camera mount alignment. No rig calibration is stored anywhere else yet (the radar
  module's README leaves radar↔EO calibration to the consumer), so defaults are 0 — nudge until
  a mark sits on its real object, then SAVE.
  - **The trim is stored on the Jetson**, not in the browser: `GET /uiprefs` reads it,
    `GET /uiprefs?set=<json>` writes it, persisted to `/var/lib/airpoc/ui-prefs.json` (falls
    back to the app's cwd if that isn't writable). It was `localStorage`, which is scoped
    **per origin** — the same rig read back different trims depending on whether the console
    was opened as `192.168.55.1` (USB), `orin-nano.lan` (WiFi) or `10.42.0.1` (field AP), and
    any new phone/laptop started at zero. Trim is a property of the **rig**, so it lives with
    the rig: one value on every device, surviving a reboot.
  - Moving a slider applies immediately but is **session-only until SAVE** (the row shows
    `unsaved` → `saved`), so nudging a trim mid-mission can't silently rewrite the stored
    alignment.
  - OVERLAY on/off stays per-browser (`localStorage`) — that's a viewing preference, not
    calibration.
- Works in replay too (video + radar both come from the recording), and the trim knobs
  stay usable there — aligning against a recording is the calibration workflow.

## EO boxes — the TRACKER is the source (console-owned render)
The **EO tracker** (`eotrack/`, `trackerd` on `:8095`) is the single source of the EO boxes the
operator sees, over **SSE `/trk/stream`**. It turns the detector's per-frame boxes into targets
with identity — stable `tid`, `state` (`tent`/`conf`/`coast`), smoothed position, and the
engaged lock. The raw detector boxes are a **DEV overlay only** (below); drawing both is the
double display this module removes.

- **Colour = class, always** (human cyan, vehicle amber) — never *how* the target was found.
  A far human promoted from faint evidence is still a human; overloading colour with provenance
  made it read as a different kind of thing.
- **Shape = state.** A confirmed/held track draws as **corner brackets** (the four corners, no
  connecting edges) instead of a plain rectangle or the seeker cross. The **engaged** track adds
  green **LOCK**. A coasting track is dashed.
- A small **`t`** at the box's top-right marks a target the detector only reported because it
  integrated faint evidence across frames (`tbd`) — a quiet provenance hint, not a recolour.
- **Tracker down → `EO TRACK · NOT CONNECTED`** on the overlay. It never silently falls back to
  raw detections: an empty frame would read as "nothing out there".
- **RAW DET** (DEV → DETECTOR) draws the detector's own boxes dim + dashed underneath,
  **off by default**. Diagnostic only — it's how you tell a tracker fault from a detector fault.
- `px` boxes are in the **native** 1440×1088 frame; the console maps them through the
  current zoom crop + the letterboxed video rect, clipped to the video content. Tapping an EO
  box hit-tests the boxes **as drawn** (smallest box containing the tap wins) rather than
  re-deriving the projection, which would drift out of sync with the render.

### Raw detector overlay (DEV only)
Boxes arrive over **SSE `/det/stream`** (~15/s) from the detection daemon (`:8094`).
`dets[]` = classified model boxes, `movers[]` = motion-only. This is the tracker's **input**.
- **MARK style** (DEV → DETECTOR → MARK): `BOX` = full bounding boxes; `SEEKER` = a heavy
  gapped cross over a dark halo on the target centroid, short labels (`V62` / `H55` /
  `M·7`). All labels get the dark halo in both modes — display-only, persisted per browser.
- **Replay**: recorded boxes push over **SSE `/rec/replay/det/stream`** (timeline-aligned,
  paced to the playback clock), with a `/rec/replay/det` poll fallback — same overlay, same
  mapping (recorded zoom for the display channel, no crop for NATIVE full-frame playback).
- Heads-up: until the trained mono model lands, the stock COCO placeholder emits false
  "vehicle" boxes on the bench — the rendering is real, today's boxes are not.

## Target list — ONE list, fusion-driven when fusion is up
When **fusion** (`fusion/`, `fusiond` on `:8096`) is delivering frames the list is drawn
**exclusively** from its `targets[]` and the panel header shows a **`FUS`** badge. Fusion down,
stale (>3 s), or replaying → the list falls back to the two per-sensor lists. Either way the
console **never dedups**: on the fusion wire a per-sensor track that appears as a constituent of
a fused row is never also published on its own, and without fusion the two sensors are simply
different objects — guessing at the association is exactly what fusion exists to stop.

| Row | Shows | Doesn't have |
|---|---|---|
| **FUS** (fused) | class · confidence · az/el **and** range — the one row type with both | — |
| **EO** (tracker) | class · confidence · azimuth/elevation | range — a camera has none |
| **RDR** (radar) | range · speed · azimuth | class — the radar emits class-less boxes |

Rows are keyed by their **constituent** — `"eo:<eo_tid>"` when there is an EO half, else
`"rad:<rad_tid>"` — not by the fusion `gid`. The video overlay and the scope draw off the
per-sensor wires at their own rate and only know sensor ids, so keying on the constituent is
what makes a tap on a row, a box and a circle land on the same target. The `gid` still carries
identity: it picks the row's colour, which therefore survives per-sensor id churn.

Persistence is **each daemon's own**: every producer runs a temporal tracker with stable ids,
confirm and coast, so rows come straight off the wire. A GUI-side hold would double-persist what
the daemons already did. Ranking pins the **engaged** target first (so the held target can never
scroll off), then fused, then confirmed EO tracks, then the rest by confidence/nearness.

## Fusion overlay — one object, one mark
The video boxes keep coming from the **tracker** at its own rate (a fusion stall must never
freeze the overlay). Each fusion frame supplies an `eo_tid → fused row` map, and that changes
two things on the video:

- A tracker box whose `tid` is a **fused constituent** draws **heavier** (in SEEKER mode, a ring
  around the cross) and its label carries the **range** the radar half brought it — a range next
  to a classified target is the whole payoff of fusion, so it goes where the operator already
  looks.
- The **radar-on-EO circle is suppressed** for any `rad_tid` that is a fused constituent. Two
  marks for one object is the double display fusion exists to end.

**Fusion angles are rig-frame already** — fusion owns the radar↔EO mount trim now — so the
console's own display trim (above) is applied only to the raw radar wire, never on top of
fusion's angles.

## Tracking — selection and mode (cluster `TRACK`)
Selecting a target **declares the tracking state**; each module then acts in its own domain off
that declaration (the tracker runs its lock loop; radar FOV, zoom and illuminator remain their
owners' jobs — the console does not reach into them).

- **Three surfaces, one result:** tap a **target-list row**, an **EO box**, or a **radar scope
  circle**. All three select the same way and switch to MANUAL first, so a pick is never
  swallowed by AUTO.
- **Two wires go out:** `trk_engage=<tid>` for an **EO** pick (the only thing the EO tracker can
  lock; `-1` clears), and the console's own `engage=<tid>` for **any** pick — so a radar-only
  selection is real published state rather than a dead click.
- **Mode/engaged are reflected FROM the tracker's wire, never from the button press.** If the
  tracker refuses or drops a lock, the console shows that truth.
- **MANUAL is the default.** AUTO takes row #1 of the merged list, but that ranking is
  **provisional** — nothing self-selects unless you switch to AUTO.
- An EO engagement is the tracker's to keep or drop; a **radar** dropout only clears a
  **radar** engagement.

## Illuminator — AUTO / MANUAL (DEV → ILLUMINATOR, `LIGHT` fires)
The **EO module owns the illuminator**; the console forwards to its `/ctl`. AUTO fits the
beam to the camera FOV at max power; MANUAL uses the PWR/BEAM sliders. `LIGHT` = `laser=0|1`.

## Record / replay / library
- **REC** (cluster) starts/stops recording via the recorder daemon (`/rec/ctl?rec=…`); on
  stop, a **save dialog** (NAME / TAGS from a bank **+ a free-text custom-tag field** /
  NOTE) → `/rec/ctl?save=…&name&tags&note`, or DISCARD.
- **REC re-attach.** If a feed is live but its recorder tap is DOWN (the shm-tap gremlin —
  recorder detached after a producer restart), REC would record 0 bytes. The button warns
  (amber pulse + tooltip) whenever `/rec/stats` `channels[].connected` is 0 for a live feed
  (eo_y10/eo_jpeg = EO, radar_raw = RADAR). A press then: (1) toasts what's down, (2) calls
  the launcher `:8088/reattach` — which runs `start.sh`, the ordered producer→recorder heal
  (a recorder-only restart does NOT re-bind and can orphan a working attach), (3) polls
  `/rec/stats` until taps re-bind, then starts recording; else toasts "press START".
- **LIBRARY** — session cards (thumbnails or a radar-only placeholder, size badges, tags,
  the note), a disk bar. Click a card → replay it. The card layout wraps the thumbnail in an
  aspect-ratio box so the name row isn't clipped in the CSS grid. Each card's name row has
  **✎ EDIT** (reopens the save dialog pre-filled with name/tags/note — incl. custom tags —
  SAVE updates the metadata via `/rec/ctl?save=`, DISCARD hidden so an edit can't delete),
  **⧉ COPY** (copies the name; http-safe clipboard fallback since the LAN/AP isn't a secure
  context), and a **Convert to HD** button: **⬆ HD** starts the recorder's background 60 fps
  transcode (`/rec/replay/transcode?sid=`, one at a time, cached, survives navigation), showing
  a live **HD N%** as it builds and **HD ✓** when ready. ✓ shows **only** for sessions whose
  HD mp4 is actually built — it reads the recorder's per-session `hd` status when present (and
  remembers ones converted this session); it is **not** keyed off `bytes.native`, which is the
  *raw* recording (present on ~every session). **Filtering:** the text search hits the recorder
  (`/rec/library?q=`) but **tag filtering is client-side** (AND-match each card's `tags`) so it
  can't be broken by query encoding — that was why filtering appeared to "lose" all annotations.
  **DELETE (n)** selected, **DELETE ALL** (double verify: confirm + type `DELETE`),
  **FREE — drop raw** per session (`purge_native`), **OFFLOAD ALL / OFFLOAD (n)** → `.tar` via
  `/rec/export`.
- The offload link is marked as a **download** (not a tab navigation) so it saves via the
  browser's download bar rather than dumping the tab on an error page if the tar build is slow.
  A plain-HTTP page can't open a folder-picker (that needs HTTPS/localhost), so it lands in
  Downloads — turn on **Chrome ▸ Settings ▸ Downloads ▸ "Ask where to save each file"** to get
  a Save-As folder window on every offload.

## DEV panel
- **STREAM** — QUALITY presets `PANIC / FAST / DEFAULT / NATIVE` + FPS CAP (forwarded to
  the EO feed's `/ctl` as `res`/`fps`; the detector always runs full-native). Measured
  bitrates: PANIC@15fps ≈ 2 Mb/s · PANIC@60 ≈ 8 · DEFAULT@60 ≈ 28 · NATIVE@60 ≈ 123 Mb/s —
  NATIVE does not fit the USB tether (~25 Mb/s) or a weak AP moment; pair it with a low
  FPS CAP. QUALITY is **always the operator's pick** — the console never changes it by itself.
  When the link chip shows **SAT** (delivered < half of produced — the "frozen EO" symptom)
  that is reported and nothing more; an earlier LINK MANUAL/AUTO mode that stepped QUALITY
  down and probed back up was removed at the operator's request (unused, and silently moving
  their setting is worse than a visibly degraded picture). **DISPLAY 30/60** (forwarded as
  `disp_fps`) caps only the *preview* rate reaching this screen — the detector, tracker,
  and recorder always run on the full sensor rate, so it never affects detection,
  targeting, or recording (distinct from FPS CAP, which sets the sensor rate). Unlike FPS
  CAP it's a hard 30/60 decimation, not a link-ease knob. The row only renders once the EO
  feed reports `disp_fps` in `/stats` (thin-proxy gate — no button for a control the feed
  can't honour); EO shipped its half, so it is live.
- **EO SENSOR** — EXPOSURE auto/man, EXP ms, GAIN, AUTO-CAP (→ EO feed `/ctl`).
  **MEDIAN and DENOISE have no controls**: the operator keeps both off, so the console simply
  asserts `median=0` + `denoise=0` on load and the panel stays uncluttered. (DENOISE is the EO
  feed's display-only night denoiser — it self-gates to high-gain night and reports `dn_active`
  / `dn_ms`; measured cost is inside the pipeline's noise floor, not the extra core it was once
  suspected of. It remains available on the feed's own `/ctl` if it is ever wanted back.)
- **ILLUMINATOR** — MODE auto/man, PWR, BEAM (→ EO feed `/ctl`).
- **RADAR ON EO** — OVERLAY on/off + AZ/EL TRIM + **SAVE** (see *Radar → EO overlay*). The
  render is client-side, but the trim is **stored on the Jetson** via `/uiprefs`.
- **DETECTOR** — MARK box/seeker (display-only), CONF (min confidence, default 0.5),
  NMS (box-merge overlap threshold, default 0.45 — lower merges duplicates harder),
  CADENCE (model runs every Nth frame, measured rate shown beside it), MOTION on/off
  (the dashed-mover safety net — **default ON** by operator request; it can flood while the
  camera itself moves until ego-motion compensation exists), MOT SENS (`mot_k`),
  MOT HOLD (`mot_persist`, 1–5 — confirmation strength: how much of the last ~1 s a mover must
  persist), **MOT MEM** (`mot_window_s`, 1–60 s step 0.5, default 15 — the motion
  rolling-background window: how far back the static scene is modelled; shorter adapts faster,
  longer forgets a stopped object slower). All except MARK forwarded namespaced `det_<key>=` →
  detection daemon `/ctl`; readback from `/dstats` (values under `knobs`). The console pushes
  **MOTION on**, **MAX DETS 25** and **TEMPORAL on** as load defaults — MAX DETS has no slider,
  it's a fixed value the operator doesn't change.
- **DETECTOR → temporal integration** — three controls that let the detector collect faint
  evidence across frames so far/weak targets get reported at all (costing ~0.2–0.4 s of extra
  delay **on those faint targets only**): **TEMPORAL** on/off (`temporal`, default **on**),
  **INTEGRATE** (`tbd_frames`, 2–20, default 6 — how long a faint target must build evidence
  before it's reported: *waiting time*, not how much is reported), and **FAINT FLOOR**
  (`tbd_lo`, 0.02–0.50, default 0.15 — how weak a hint is worth collecting; **this** is the knob
  that decides how much gets reported). Detections carrying `tbd:1` were promoted by integration
  and are drawn with a **violet outline + `·t`** — they carry the extra latency, so they're the
  ones to sanity-check first.
  - > **Pitfall:** FAINT FLOOR turns red and warns above **0.25**, and that number is tied to the
    > *stock placeholder model*, where people sit at the very bottom of the confidence range —
    > raising it thins clutter but **deletes people**. When the trained model lands the table must
    > be re-measured and this threshold moved; the detection module's README names that obligation.
    > A warning at a level that no longer means anything is worse than no warning.
  - > **Pitfall:** the detector's `/ctl` answers `ok`/200 to *any* request and silently ignores
    > parameters it doesn't know, so a 200 proves the forward arrived — never that the running
    > build honours it. `/stats` → `knobs.<name>` is the only real presence test. That's why these
    > rows stay hidden until `knobs.temporal` appears (see *Gated controls* below).
- **RADAR** — the daemon's **ten** tracker knobs: FOV ± (azimuth gate), **EL ±** (`elmax` —
  elevation gate, 5–90°, the antenna's physical elevation beam edge; 90 = off), MIN SNR,
  MIN SPD, MERGE GATE (`doppler` — velocity-coherence for the dedup merge), DEDUP (`eps` —
  merge radius for co-located boxes), MIN PTS, CONFIRM (M-of-N hits before a track appears,
  1–6), COAST (seconds a track survives a dropout, 0–3), PARK HOLD (seconds a stopped mover is
  held, 0–60). All forward to the **daemon** namespaced `radar_<key>=` (the app strips the
  prefix → daemon `/ctl`); sliders read back the **applied (clamped)** value from the daemon's
  `/stats`. The console pushes the operator's chosen radar defaults on load: **FOV ±60°** and
  **EL ±20°**. (These were briefly *not* pushed — an earlier auto-`radar_fov=60` was removed
  because it overrode the daemon mid-test — but the operator asked for a defined default, so
  they are asserted again. Nothing else on the radar is touched.)
- **SYSTEM** — Jetson **TEMP** (junction/`tj` thermal zone), **CPU %** (aggregate, plus
  core-equivalents `x/N`), **GPU %** (Tegra load). RETURN TO CONTROL → the launcher (`:8088`).

## Gated controls (don't ship a button the daemon can't honour)
A control for a knob the *running* daemon doesn't implement is worse than no control: it looks
operable, changes nothing, and there's no feedback saying so. So a control for a not-yet-shipped
knob stays hidden until the daemon proves it exists, then appears by itself — no console change
needed at flash time. Used today by **DISPLAY 30/60** (`eo.disp_fps`) and the three **temporal**
rows (`knobs.temporal`).

- **Test presence with `/stats`, never with a `/ctl` 200.** Every feed's `/ctl` returns `ok`/200
  and silently ignores parameters it doesn't know, so a 200 proves only that the forward arrived.
  The knob appearing in `/stats` is the only real evidence.
- **Forward the key regardless.** It's harmless on an old build (ignored) and means the control
  works the instant the daemon ships it.
> **Pitfall:** `.srow` sets `display:grid`, which **overrides the `hidden` attribute's**
> `display:none` — a row gated with `hidden` alone renders anyway. `.srow[hidden]{display:none}`
> restores it. This silently exposed the temporal rows on a detector build that had none of those
> knobs; the symptom is a visible row with *no* option highlighted, because the readback that
> selects one only runs when the knob exists.

## HTTP endpoints (the app)
| Path | Purpose |
|---|---|
| `/` · `/app.css` · `/app.js` | the embedded page (served **`no-store`** — never cached, so a deploy's new UI shows on the next reload without a hard-refresh) |
| `/stream` | EO module's JPEG frames **relayed** as MJPEG multipart, fanned to every screen |
| `/radar` | the radar daemon's latest frame **verbatim** (one-shot poll; used in replay) |
| `/radar/stream` | **SSE** push of each radar frame at native rate (`data: {…}`); the live path |
| `/det/stream` | **SSE** push of each detector message (~15/s); the live det-box path |
| `/det` | the detector's latest message verbatim (one-shot poll) |
| `/dstats` | the detector's `/stats` (health + `knobs`) for slider init |
| `/trk/stream` | **SSE** push of each EO-tracker message; **the** EO-box path (tracks with identity + engaged lock) |
| `/trk` | the tracker's latest message verbatim (one-shot poll) |
| `/tstats` | the tracker's `/stats` (health + `knobs`) |
| `/fus/stream` | **SSE** push of each fusion message (~41/s with both trackers up); drives the target list |
| `/fus` | fusion's latest message verbatim (one-shot poll) |
| `/fstats` | fusion's `/stats` (health, feeds, trim + `knobs`) |
| `/rstats` | the radar daemon's `/stats` (its control values + fps/drops) for slider init |
| `/uiprefs` | operator UI prefs stored **on the Jetson** (`/var/lib/airpoc/ui-prefs.json`): plain `GET` reads, `?set=<url-encoded json>` writes. Currently the radar→EO AZ/EL trim — rig-level state that must not depend on which browser or which IP the console was opened from. Deliberately schema-less: the console stores the blob the page hands it, so new prefs need no C change |
| `/stats` | console state + the EO feed's `/stats` nested under `"eo"` |
| `/ctl?…` | routed: `track`/`engage` → local; `radar_*` → radar daemon; `det_*` → detection daemon; `trk_*` → the EO tracker; `fus_*` → fusion (`trim_az/trim_el/gate/confirm/divorce_s/coast_s`); the rest (`zoom/laser/power/fov/ae/gain/expms/gaincap/median/denoise/disp_fps/fps/res`) → the EO feed |
| `/rec/<path>` | **pass-through** to the recorder daemon (`:8093`): `/rec/ctl`, `/rec/library`, `/rec/thumbs/…`, `/rec/export` (offload `.tar`), `/rec/replay/*` (incl. `/rec/replay/state` with `native_mp4` status, `/rec/replay/{radar,det}/stream` SSE, `/rec/replay/transcode?sid=` HD build) |

The `/rec/*` proxy sets a **180 s** upstream read timeout (a slow `/rec/export` tar build can
take a while before the first byte) and detects client-disconnect on write.

`/stats` top-level: `eo_connected`, `mbps`, `tx_fps`, `link_type`, `rssi_dbm`, `link_total_mbps`,
`cpu_c` (Jetson junction °C), `cpu_pct`, `gpu_pct`, `ncpu`, `track`, `engage`, `tracks`;
reserved `null`: `batt`, `alt`, `brg`, `rng`; plus **`eo`** — the EO feed's `/stats` object
(`fps`, `sfps`, `hfov`, `vfov`, `zoom`, `laser`, `lpower`, `lfov`, `lpresent`, `gain`,
`exp_ms`, `median`, `ae`, `gaincap`, `eff_w/eff_h`, …) or `null` when the feed is down.

## Notes
- **No synthetic data.** A feed that isn't up shows **NOT CONNECTED**. Never run the radar
  daemon with `-s` in front of an operator.
- **Bitrate is the EO module's.** DEV forwards the two levers (`res`, `fps`) to the EO
  feed; on a marginal link, drop QUALITY / FPS CAP. Format is software MJPEG (the Orin Nano
  has no NVENC/NVJPG); the app only relays bytes.
- **"Recorder tap" ≠ "feed up".** A feed's port can be up (live view fine) while its
  `/dev/shm/airpoc.*` tap is gone → recordings come out empty. The launcher's `/status`
  reports this honestly (`eo_rec`/`radar_rec`); press START to re-attach.
- **Feed reads time out (5 s), on purpose.** Every client socket carries `SO_RCVTIMEO`. Two
  things depend on it: the console can be **stopped** (a reader parked in an unbounded `read()`
  never re-reads its run flag, so the process used to hang after SIGTERM while still consuming
  every feed), and a **wedged** daemon — connection open, nothing sent — ends the session and
  shows NOT CONNECTED instead of a frozen picture that looks live. 5 s sits far above every
  feed's message interval (EO 60 Hz, radar ~26 Hz, detector/tracker 15 Hz, fusion ≥1 Hz
  heartbeat), so a working connection is never recycled. If a feed ever legitimately goes quiet
  for longer than that, the symptom is a reconnect every 5 s — raise this before muting it.

> Pitfall: `LIGHT` is live-fire (invisible 850 nm, eye hazard). There is **no**
> confirmation step — one click emits immediately, and the device resets to MAX
> power every time it is switched on. Treat the aperture as live.
