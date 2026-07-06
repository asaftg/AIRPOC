# AIRPOC launcher — start/stop from any device

A tiny always-on web page that starts and stops the operator stack. It runs as its own
systemd service so it is up whenever the Jetson is up; from it you press **START SYSTEM**
to bring up EO + radar + console, and **STOP SYSTEM** to bring them down. Reachable from
any phone/laptop/tablet on the network — no install, no SSH.

```
  browser (any device)                Jetson (orin-nano)
  +--------------------+   :8088      +------------------------------+
  |  FAZE CONTROL page | -----------> |  airpoc-launcher  (always on)|
  |  START / STOP /    |              |    /start -> start.sh        |
  |  OPEN CONSOLE      | <-- /status  |    /stop  -> stop.sh         |
  +--------------------+              |    /status -> port probes    |
                                      +------------------------------+
                                         | start.sh brings up:
                                         |- eo_pipeline   :8091
                                         |- radar_preview :8092
                                         +- app (console) :8080
```

The launcher never dies with the stack — STOP leaves the launcher (and the recorder,
its own service) running so you can always press START again.

## Install (once, on the Jetson)

```
cd ~/AIRPOC && git pull
cd app/launcher && ./install.sh
```

It compiles `airpoc-launcher`, installs the systemd unit, and enables it. Prints the URL.

## Use (from any device)

1. Open **http://orin-nano.lan:8088/** (or **http://192.168.86.101:8088/**).
   Bookmark it, or use the desktop shortcut in `desktop/`.
2. **START SYSTEM** — brings up EO, radar, console. The status dot goes green.
3. **OPEN CONSOLE** — opens the operator GUI (`:8080`) in a new tab.
4. **STOP SYSTEM** — shuts the stack down. The launcher stays up.

## Desktop shortcut

`desktop/AIRPOC-Control.url` (Windows) and `desktop/AIRPOC-Control.desktop` (Linux) just
open the control page. Copy one to your desktop and double-click.

## Files

| file | what |
|---|---|
| `airpoc-launcher.c`       | the always-on :8088 control server + embedded page |
| `airpoc-launcher.service` | systemd unit (User=asaftg, Restart=always) |
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
