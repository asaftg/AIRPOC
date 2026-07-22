#!/bin/sh
# Install the fusiond systemd unit (run on the Jetson, from fusion/systemd/).
set -e
sudo cp airpoc-fusion.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable airpoc-fusion.service
echo "installed; start with: sudo systemctl start airpoc-fusion"
