#!/bin/bash
# IMX296 quality preview (AE over i2c + light ISP). Needs i2c access:
# the asaftg user is in the 'i2c' group, so no sudo required on a fresh login.
pkill -f imx296_preview 2>/dev/null; pkill -f imx296_focus_web 2>/dev/null; pkill -9 v4l2-ctl 2>/dev/null; sleep 1
nohup python3 "$HOME/imx296_preview.py" > "$HOME/preview.log" 2>&1 &
sleep 4
echo "Quality preview: http://$(hostname -I | tr ' ' '\n' | grep -E '^192\.168\.86\.' | head -1):8091"
