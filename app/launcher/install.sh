#!/bin/bash
# Install the AIRPOC launcher on the Jetson (run once). Builds the control server and
# registers the always-on systemd service so it survives reboots and comes up on boot.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

cc -O2 -Wall -o "$DIR/airpoc-launcher" "$DIR/airpoc-launcher.c"
chmod +x "$DIR/start.sh" "$DIR/stop.sh"

sudo cp "$DIR/airpoc-launcher.service" /etc/systemd/system/airpoc-launcher.service
sudo systemctl daemon-reload
sudo systemctl enable airpoc-launcher
sudo systemctl restart airpoc-launcher   # restart (not just enable) so a re-install loads the new binary

echo
echo "Launcher installed and running."
echo "  control page:  http://$(hostname):8088/   (bookmark this on any device)"
echo "  console:       http://$(hostname):8080/   (opens from the control page once started)"
