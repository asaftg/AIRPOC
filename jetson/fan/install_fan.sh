#!/bin/bash
# Run on Jetson as root: install + enable the always-100% fan service.
install -m 0755 /tmp/fan-max.sh /usr/local/bin/fan-max.sh
install -m 0644 /tmp/jetson-fan-max.service /etc/systemd/system/jetson-fan-max.service
systemctl stop nvfancontrol 2>/dev/null
systemctl disable nvfancontrol 2>/dev/null
systemctl daemon-reload
systemctl enable --now jetson-fan-max.service
sleep 2
echo "=== fan service ==="; systemctl is-active jetson-fan-max.service
echo "=== pwm values (expect 255) ==="
for p in $(find /sys -name pwm1 -path '*hwmon*' 2>/dev/null); do echo "$p = $(cat $p 2>/dev/null)"; done
echo "=== tach (fan rpm if available) ==="; cat /sys/devices/platform/pwm-fan/hwmon/hwmon*/rpm 2>/dev/null
echo FAN_DONE
