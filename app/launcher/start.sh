#!/bin/bash
# Start the AIRPOC stack — EO preview (:8091), radar daemon (:8092), operator console
# (:8080). Detaches each child (setsid) so it outlives this script. Waits for USB devices
# that enumerate late. Overridable via env: AIRPOC_BASE, EO_VIDEO, EO_ILLUM.
BASE="${AIRPOC_BASE:-$HOME/AIRPOC}"
EO_VIDEO="${EO_VIDEO:-/dev/video0}"
EO_ILLUM="${EO_ILLUM:-/dev/ttyUSB0}"

# Serialize concurrent starts. /start, /resume and /reattach all shell out to this script and
# can fire close together; two copies racing on ensure_gone/launch (or the recorder bounce)
# duplicate producers or unlink each other's shm -> empty recordings. First one wins; the rest
# bail (the running start already does the work).
exec 9>/tmp/airpoc-start.lock
flock -n 9 || { echo "start.sh: another start already in progress — skipping"; exit 0; }

up()       { ss -ltn 2>/dev/null | grep -q ":$1 "; }              # is a TCP port bound?
wait_dev() { local d="$1" n="${2:-15}"; for _ in $(seq 1 "$n"); do [ -e "$d" ] && return 0; sleep 1; done; return 1; }

# A producer is HEALTHY only if its port is bound AND its shared-memory tap exists. If the
# tap was unlinked out from under a still-running producer (the restart race), it's
# up-but-tap-less: the recorder can't attach and recordings come out empty — so we treat it
# as unhealthy and cleanly restart it.
healthy() { up "$1" && [ -e "$2" ]; }                            # port  tapfile

# Kill any instance matching $1 and WAIT for it to fully exit, so its shm_unlink() on
# shutdown finishes BEFORE we launch a fresh producer — otherwise the dying old process
# unlinks the shm name the new one just created, and the recorder loses the feed.
# Match by EXACT process name (-x), never -f substring: `pkill -f eo_pipeline` would also hit a
# log tail, an ssh session, or an in-flight start.sh whose argv merely contains the word.
ensure_gone() {
  pgrep -x "$1" >/dev/null 2>&1 || return 0
  echo "clearing stale $1 ..."
  pkill -x "$1" 2>/dev/null
  for _ in $(seq 1 25); do pgrep -x "$1" >/dev/null 2>&1 || { sleep 0.3; return 0; }; sleep 0.2; done
  pkill -9 -x "$1" 2>/dev/null; sleep 0.5
}

# NOTE the `9>&-` on the subshell: fd 9 is the start lock (above). Open fds are inherited
# across fork/exec, and a flock lives as long as ANY fd on that open file description stays
# open — so without this every daemon we launch (and its children) would hold the lock for
# its whole life. The next start.sh would then hit `flock -n`, say "another start already in
# progress" and silently do nothing: /reattach, /resume, and START-to-recover-one-dead-producer
# would all become no-ops. Closing it on the SUBSHELL (not just on `setsid`) is what actually
# works — the intermediate shell holds fd 9 too. Verified: after this, no process holds the
# lock once start.sh exits, while two genuinely concurrent starts are still serialized.
launch() { # port  dir  cmd...
  local port="$1" dir="$2"; shift 2
  [ -d "$dir" ] || { echo ":$port SKIP — missing $dir"; return 1; }
  echo "starting :$port  ($*)"
  ( cd "$dir" && setsid "$@" >"/tmp/airpoc-$port.log" 2>&1 </dev/null & ) 9>&-
}

restarted=0

# --- EO producer (shm tap: airpoc.eo_y10) ---
if healthy 8091 /dev/shm/airpoc.eo_y10; then
  echo ":8091 healthy — skip"
else
  ensure_gone "eo_pipeline"
  wait_dev "$EO_VIDEO" 10 || echo "warn: $EO_VIDEO absent — EO may not stream"
  EO_ARGS=(-d "$EO_VIDEO" -p 8091); [ -e "$EO_ILLUM" ] && EO_ARGS+=(-i "$EO_ILLUM")
  launch 8091 "$BASE/eo/pipeline" ./eo_pipeline "${EO_ARGS[@]}" && restarted=1
fi

# --- Radar producer (shm tap: airpoc.radar_raw). Daemon defaults to /dev/radar-cli|data
# udev symlinks that aren't installed here, so resolve the AWR XDS110 UARTs by stable
# by-id path (if00=CLI, if03=data) and pass the cfg (it lives in radar/cfg). Board
# enumerates a few seconds after boot — wait for the CLI port. ---
if healthy 8092 /dev/shm/airpoc.radar_raw; then
  echo ":8092 healthy — skip"
else
  ensure_gone "radar_preview"
  RCLI=$(ls /dev/serial/by-id/*XDS110*if00* 2>/dev/null | head -1); RCLI="${RCLI:-/dev/ttyACM0}"
  RDAT=$(ls /dev/serial/by-id/*XDS110*if03* 2>/dev/null | head -1); RDAT="${RDAT:-/dev/ttyACM1}"
  if wait_dev "$RCLI" 15; then
    launch 8092 "$BASE/radar/src" ./radar_preview -C "$RCLI" -D "$RDAT" -c ../cfg/awr2944P_ag.cfg -w ../web && restarted=1
  else
    echo ":8092 SKIP — radar board not on USB (no XDS110 CLI port)"
  fi
fi

# --- Detection consumer (reads airpoc.eo_y10 read-only, serves :8094, shm tap:
# airpoc.det_wire). EO object detector: publishes per-frame target boxes for the console
# and tracker over /stream + /stats + /ctl. No device of its own; needs the EO tap but
# self-heals if EO isn't up yet, so launch order after EO is enough. The engine (built
# on-device by trtexec, gitignored under /data) is passed with -e; if it's missing the
# daemon runs model-less (motion + heartbeat) rather than failing. ---
DET_ENGINE="${DET_ENGINE:-/data/detection/engines/rtmdet-t-raw.fp16.trt10.engine}"
if healthy 8094 /dev/shm/airpoc.det_wire; then
  echo ":8094 healthy — skip"
else
  ensure_gone "detectiond"
  launch 8094 "$BASE/detection" ./detectiond -p 8094 -e "$DET_ENGINE" && restarted=1
fi

sleep 2   # let the feeds bind before the console dials into them

# --- Operator console (consumer; no shm tap) ---
if up 8080; then echo ":8080 already up — skip"
else launch 8080 "$BASE/app" ./app -p 8080 -e 127.0.0.1:8091 -r 127.0.0.1:8092 -c 127.0.0.1:8093 -d 127.0.0.1:8094; fi

# Recorder re-attach: if we (re)started a producer, its shm is fresh; bounce the always-on
# recorder so its taps bind to the live feeds (else it records 0 bytes). Only on a real
# (re)start, so a redundant START never interrupts an in-progress recording. Scoped
# NOPASSWD rule from app/launcher/install.sh.
if [ "$restarted" -ne 0 ]; then
  sleep 3   # let the feeds create + populate their shm first
  echo "feeds (re)started -> restarting recorder so its taps re-attach"
  sudo -n systemctl restart airpoc-recorder 2>/dev/null || echo "warn: recorder not restarted (missing NOPASSWD sudoers rule)"
fi

echo "start requested"
