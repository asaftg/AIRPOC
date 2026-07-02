#!/bin/sh
# Install + start the AIRPOC operator console as a systemd service on the Jetson.
# Generates the unit from the template with this checkout's real paths (no hand-
# edits diverging from the repo). Mirrors jetson/fan/install_fan.sh.
set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"     # the app/ directory
BIN="$DIR/app"
USR="${SUDO_USER:-$USER}"

[ -x "$BIN" ] || { echo "build first:  (cd $DIR && make)"; exit 1; }

UNIT=/etc/systemd/system/airpoc-app.service
sed "s#@DIR@#$DIR#g; s#@BIN@#$BIN#g; s#@USER@#$USR#g" \
    "$DIR/systemd/airpoc-app.service.in" | sudo tee "$UNIT" >/dev/null

sudo systemctl daemon-reload
sudo systemctl enable --now airpoc-app
echo "airpoc-app installed and started — http://<jetson>:8080/"
