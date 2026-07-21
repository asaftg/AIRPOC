#!/bin/bash
# install the EO tracker service from the persistent clone (run with sudo)
set -euo pipefail
cd "$(dirname "$0")"
( cd .. && make )
install -m 644 airpoc-tracker.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now airpoc-tracker
systemctl --no-pager status airpoc-tracker | head -5
