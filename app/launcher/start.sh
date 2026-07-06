#!/bin/bash
# Start the AIRPOC stack — EO preview (:8091), radar daemon (:8092), operator console
# (:8080). Idempotent: skips anything already listening. Detaches each child (setsid) so
# it outlives this script and the launcher. Waits for USB devices that enumerate late.
#
# Overridable via env: AIRPOC_BASE, EO_VIDEO, EO_ILLUM.
BASE="${AIRPOC_BASE:-$HOME/AIRPOC}"
EO_VIDEO="${EO_VIDEO:-/dev/video0}"
EO_ILLUM="${EO_ILLUM:-/dev/ttyUSB0}"

up()       { ss -ltn 2>/dev/null | grep -q ":$1 "; }              # is a TCP port bound?
wait_dev() { local d="$1" n="${2:-15}"; for _ in $(seq 1 "$n"); do [ -e "$d" ] && return 0; sleep 1; done; return 1; }
start()  { # port  dir  cmd...
  local port="$1" dir="$2"; shift 2
  if up "$port"; then echo ":$port already up — skip"; return; fi
  if [ ! -d "$dir" ]; then echo ":$port SKIP — missing $dir"; return; fi
  echo "starting :$port  ($*)"
  ( cd "$dir" && setsid "$@" >"/tmp/airpoc-$port.log" 2>&1 </dev/null & )
}

# EO preview — links libeo, serves MJPEG + /stats + /ctl on :8091. Illuminator (-i) is
# optional; if the serial node is absent, EO still comes up for video.
wait_dev "$EO_VIDEO" 10 || echo "warn: $EO_VIDEO absent — EO may not stream"
EO_ARGS=(-d "$EO_VIDEO" -p 8091)
[ -e "$EO_ILLUM" ] && EO_ARGS+=(-i "$EO_ILLUM")
start 8091 "$BASE/eo/pipeline" ./eo_pipeline "${EO_ARGS[@]}"

# Radar daemon — serves SSE on :8092. The daemon defaults to /dev/radar-cli|radar-data
# (udev symlinks that aren't installed here), so resolve the AWR's XDS110 UARTs by their
# stable by-id path: if00 = CLI/config UART, if03 = high-speed data. The cfg lives in
# radar/cfg (not radar/src), so pass it explicitly. The board enumerates a few seconds
# after boot — wait for the CLI port before starting, else it gets ENOENT and gives up.
RCLI=$(ls /dev/serial/by-id/*XDS110*if00* 2>/dev/null | head -1); RCLI="${RCLI:-/dev/ttyACM0}"
RDAT=$(ls /dev/serial/by-id/*XDS110*if03* 2>/dev/null | head -1); RDAT="${RDAT:-/dev/ttyACM1}"
if wait_dev "$RCLI" 15; then
  start 8092 "$BASE/radar/src" ./radar_preview -C "$RCLI" -D "$RDAT" -c ../cfg/awr2944P_ag.cfg -w ../web
else
  echo ":8092 SKIP — radar board not on USB (no XDS110 CLI port)"
fi

sleep 2   # let the feeds bind before the console dials into them

# Operator console — pure proxy over EO (:8091) + radar (:8092) + recorder (:8093).
start 8080 "$BASE/app" ./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092 -c 127.0.0.1:8093

echo "start requested"
