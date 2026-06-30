#!/bin/bash
pkill -9 v4l2-ctl 2>/dev/null; pkill -f imx296_focus_web 2>/dev/null; sleep 1
nohup python3 "$HOME/imx296_focus_web.py" > /tmp/focus.log 2>&1 &
sleep 3
echo "Focus assist: http://$(hostname -I | tr ' ' '\n' | grep -E '^192\.168\.86\.' | head -1):8090"
