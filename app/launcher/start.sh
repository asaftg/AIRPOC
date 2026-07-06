#!/bin/bash
# Start the AIRPOC stack — EO preview (:8091), radar daemon (:8092), operator console
# (:8080) — using each module's documented invocation. Idempotent: skips anything already
# listening. Detaches each child (setsid) so it outlives this script and the launcher.
#
# Overridable via env: AIRPOC_BASE, EO_VIDEO, EO_ILLUM (illuminator serial, optional).
BASE="${AIRPOC_BASE:-$HOME/AIRPOC}"
EO_VIDEO="${EO_VIDEO:-/dev/video0}"
EO_ILLUM="${EO_ILLUM:-/dev/ttyUSB0}"

up()    { ss -ltn 2>/dev/null | grep -q ":$1 "; }          # is a TCP port already bound?
start() { # port  dir  cmd...
  local port="$1" dir="$2"; shift 2
  if up "$port"; then echo ":$port already up — skip"; return; fi
  if [ ! -d "$dir" ]; then echo ":$port SKIP — missing $dir"; return; fi
  echo "starting :$port  ($*)"
  ( cd "$dir" && setsid "$@" >"/tmp/airpoc-$port.log" 2>&1 </dev/null & )
}

# EO preview — links libeo, serves MJPEG + /stats + /ctl on :8091. Illuminator (-i) is
# optional; if the serial node is absent, EO still comes up for video.
EO_ARGS=(-d "$EO_VIDEO" -p 8091)
[ -e "$EO_ILLUM" ] && EO_ARGS+=(-i "$EO_ILLUM")
start 8091 "$BASE/eo/pipeline" ./eo_pipeline "${EO_ARGS[@]}"

# Radar daemon — pushes the A/G cfg, grabs the ACM ports itself, serves SSE on :8092.
start 8092 "$BASE/radar/src" ./radar_preview -w ../web

sleep 2   # let the feeds bind before the console dials into them

# Operator console — pure proxy over EO (:8091) + radar (:8092) + recorder (:8093).
start 8080 "$BASE/app" ./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092 -c 127.0.0.1:8093

echo "start requested"
