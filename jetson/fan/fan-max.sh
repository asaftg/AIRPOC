#!/bin/bash
# Force the Jetson fan to 100% always: hold the pwm-fan cooling device at max_state
# AND write pwm=255. Re-asserted every 1s so the thermal governor can't idle it off.
systemctl stop nvfancontrol 2>/dev/null
FANCD=""
for cd in /sys/class/thermal/cooling_device*; do
  [ "$(cat "$cd/type" 2>/dev/null)" = "pwm-fan" ] && FANCD="$cd"
done
MAX=$(cat "$FANCD/max_state" 2>/dev/null); [ -z "$MAX" ] && MAX=3
PWM=/sys/devices/platform/pwm-fan/hwmon/hwmon0/pwm1
while true; do
  [ -n "$FANCD" ] && echo "$MAX" > "$FANCD/cur_state" 2>/dev/null
  echo 255 > "$PWM" 2>/dev/null
  sleep 1
done
