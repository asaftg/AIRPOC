#!/bin/bash
# Run on Jetson as root: install + enable the clocks-pin service (persists across reboots).
install -m 0755 /tmp/pin-clocks.sh /usr/local/bin/pin-clocks.sh
install -m 0644 /tmp/jetson-clocks.service /etc/systemd/system/jetson-clocks.service
systemctl daemon-reload
systemctl enable jetson-clocks.service
# restart (not just enable --now): a oneshot with RemainAfterExit stays "active (exited)"
# after its first run, so enable --now would NOT re-execute an updated script.
systemctl restart jetson-clocks.service
sleep 3
echo "=== clocks service ==="; systemctl is-active jetson-clocks.service
echo "=== nvpmodel ==="; nvpmodel -q 2>/dev/null | grep -iE 'mode|maxn' | head -2
echo "=== GPU cur / max freq (expect cur == max under load) ==="
echo "cur=$(cat /sys/class/devfreq/17000000.gpu/cur_freq 2>/dev/null)  max=$(cat /sys/class/devfreq/17000000.gpu/max_freq 2>/dev/null)"
echo "=== CPU governor (expect performance) ==="
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo "=== thermal zones (junction temps, C) ==="
for t in /sys/class/thermal/thermal_zone*/temp; do
    d=${t%/temp}; v=$(cat "$t" 2>/dev/null); [ -n "$v" ] && echo "  $(cat $d/type)=$(( v/1000 ))C"
done
echo CLOCKS_DONE
