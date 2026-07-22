# AIRPOC launcher — start/stop from any device

A tiny always-on web page that starts/stops the operator stack, switches WiFi mode, and
powers the box off. It runs as its own systemd service so it's up whenever the Jetson is
up. Reachable from any phone/laptop/tablet on the network — no install, no SSH.

```
  browser (any device)                Jetson (orin-nano)
  +----------------------+ :8088      +----------------------------------+
  |  FAZE CONTROL page   | ---------> |  airpoc-launcher  (always on)    |
  |  START / STOP        |            |   /start -> start.sh             |
  |  OPEN CONSOLE        | <-/status- |   /stop  -> stop.sh              |
  |  NETWORK AUTO/WIFI/AP|            |   /status -> feed ports + recorder|
  |  SHUTDOWN JETSON     |            |              tap health           |
  +----------------------+            |   /wifi[/status] -> mode file     |
                                      |   /reattach -> start.sh (REC heal)|
                                      |   /suspend /resume -> stop/start  |
                                      |               the live producers   |
                                      |   /shutdown -> systemctl poweroff |
                                      +----------------------------------+
                                         | start.sh brings up (self-healing):
                                         |- eo_pipeline   :8091
                                         |- radar_preview :8092
                                         |- detectiond    :8094
                                         |- trackerd      :8095
                                         |- fusiond       :8096
                                         +- app (console) :8080
```

- **Launch order matters.** `trackerd` consumes the detector, so it starts **after**
  `detectiond`, and `fusiond` consumes the radar tracker + `trackerd`, so it starts after
  both; the console starts last and dials into all of them. Fusion reconnects on its own, so
  its order only decides how fast the fused picture fills in — and the console falls back to
  the per-sensor lists whenever it is absent. The tracker is a pure
  consumer (no device, no shm tap of its own), so a bound port is its whole health check —
  unlike the producers, which are only healthy when the port **and** their `/dev/shm` tap exist.
- **Honest status.** `/status` returns `eo`/`radar`/`det`/`fus` (feed ports) **and** `eo_rec`/
  `radar_rec` (does the recorder's `/dev/shm` tap actually flow?). A feed can be up while
  its tap is dead → recordings come out empty; the page then shows **REC BUS DOWN** instead
  of a green ✓, and `start.sh` re-attaches on the next START (it counts a feed healthy only
  when port **and** tap exist, and bounces the recorder after (re)starting a feed).
- **NETWORK toggle** (AUTO/WIFI/AP) and the field failover live in
  [`jetson/wifi-ap/`](../../jetson/wifi-ap/README.md) — the launcher just writes the mode.
- **`/reattach`** re-binds the recorder's shm taps on demand — the console's REC button
  calls it when a feed is live but its tap is down, running the same ordered `start.sh` heal
  (a recorder-only restart doesn't re-bind and can orphan a working attach).
- **`/suspend` / `/resume`** stop / relaunch the live producers (eo_pipeline, radar_preview,
  detectiond) — clean SIGTERM + `start.sh`, **not** SIGSTOP (a frozen camera/USB process
  corrupts its device state and dies on resume). **The console no longer drives these** — an
  earlier "suspend the box while reviewing recordings" feature was removed because restarting
  the camera + detector on every library visit thrashed the box (CPU pegged, camera wedged
  after a few enter/exit cycles). The endpoints remain for manual use / crash recovery; the
  console's live-view backstop calls `/resume` only if EO is genuinely down.
- **SHUTDOWN JETSON** → `/shutdown` → `sudo -n systemctl poweroff` (a scoped NOPASSWD rule
  installed by `install.sh`). Confirms first.

The launcher never dies with the stack — STOP leaves the launcher (and the recorder,
its own service) running so you can always press START again.

**Two things that used to break this, now fixed:**
- **The heal path was silently dead.** `start.sh` takes a lock so two starts can't race. That
  lock lived on a file descriptor which every daemon it launched inherited and held for life, so
  the *next* start.sh always concluded "a start is already in progress" and did nothing. STOP →
  START still worked (STOP kills the holders), but healing **without** a stop — `/reattach`,
  `/resume`, and pressing START to recover one dead producer while the others ran — quietly did
  nothing. The lock is now closed in the launched children (`9>&-` on the launch subshell), so
  it dies with start.sh while two genuinely concurrent starts are still serialized.
- **One silent visitor wedged the whole service.** The server handles one client at a time and
  used to block forever on a client that connected and sent nothing — a captive-portal probe, a
  speculative browser connection, a port scan. Since it also binds :80 for captive-portal checks,
  that was routine. Accepted sockets now carry a send/receive deadline and the request is read to
  completion or timed out, so START/STOP/SHUTDOWN stay reachable. `/stop` also runs in the
  background now; it used to freeze every other request for the duration of the shutdown.

## Install (once, on the Jetson)

```
cd ~/AIRPOC && git pull
cd app/launcher && ./install.sh
```

It compiles `airpoc-launcher`, installs the systemd unit, and enables it. Prints the URL.

## Use (from any device)

1. Open **http://orin-nano.lan:8088/** (or **http://192.168.86.101:8088/**).
   Bookmark it, or use the desktop shortcut in `desktop/`.
2. **START SYSTEM** — brings up EO, radar, console. The status dot goes green (amber +
   "REC BUS DOWN" if a feed is live but its recorder tap isn't — press START again to heal).
3. **OPEN CONSOLE** — opens the operator GUI (`:8080`) in a new tab.
4. **STOP SYSTEM** — shuts the stack down. The launcher + recorder stay up.
5. **NETWORK** — AUTO (home WiFi on the bench, AP in the field) / WIFI (pin home) / AP (pin
   the field access point). Switching drops your current network — rejoin the new one.
6. **SHUTDOWN JETSON** — powers the box off (physical access needed to turn it back on).

## Desktop shortcut

`desktop/AIRPOC-Control.url` (Windows) and `desktop/AIRPOC-Control.desktop` (Linux) just
open the control page. Copy one to your desktop and double-click.

## Files

| file | what |
|---|---|
| `airpoc-launcher.c`       | the always-on :8088 control server + embedded page |
| `airpoc-launcher.service.in` | systemd unit template (user/home filled in by `install.sh`) |
| `start.sh`                | idempotent stack start (eo/radar/app, skips what's up) |
| `stop.sh`                 | stops eo/radar/app; leaves launcher + recorder |
| `install.sh`              | build + install + enable the service |
| `desktop/`                | double-click shortcuts to the control page |

## Notes

- START/STOP do **not** touch the recorder (`airpoc-recorder`, its own service) or this
  launcher — recordings and control survive a stack restart.
- The stack does **not** auto-start on boot by design — you decide when it's live. Only
  the launcher auto-starts, so control is always there.
- `start.sh` reads `AIRPOC_BASE` (default `~/AIRPOC`), `EO_VIDEO`, `EO_ILLUM` if the repo
  or device nodes live elsewhere. No simulation flags — real hardware only.
