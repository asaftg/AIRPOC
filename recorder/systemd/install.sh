#!/bin/bash
# install the recorder service from the persistent clone (run with sudo)
set -euo pipefail
cd "$(dirname "$0")"
( cd ../src && make )
install -m 644 airpoc-recorder.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now airpoc-recorder
systemctl --no-pager status airpoc-recorder | head -5
