#!/bin/sh
# AIRPOC laptop launcher (Linux/macOS) — start/stop the Jetson service and open the
# operator console. Override host/user/port via env: AIRPOC_HOST, AIRPOC_USER,
# AIRPOC_PORT.  Usage:  airpoc.sh [start|stop|open]
HOST=${AIRPOC_HOST:-192.168.55.1}
USER=${AIRPOC_USER:-asaftg}
PORT=${AIRPOC_PORT:-8080}
ACTION=${1:-start}

[ "$ACTION" = "open" ] || ssh "$USER@$HOST" "sudo systemctl $ACTION airpoc-app"
case "$ACTION" in
  start|open)
    (xdg-open "http://$HOST:$PORT/" >/dev/null 2>&1 || open "http://$HOST:$PORT/") & ;;
esac
