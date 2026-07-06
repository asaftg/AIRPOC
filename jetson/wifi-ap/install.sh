#!/bin/bash
# Install the AIRPOC WiFi failover on the Jetson (run once). Creates the open point-to-
# point access point profile and the failover service. Safe to run while connected over
# the home WiFi: the AP profile is autoconnect=off and the service prefers the home
# network whenever it is in range, so it will NOT drop your current link.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
IFACE="${AIRPOC_WIFI_IFACE:-wlP1p1s0}"
AP_CON="AIRPOC-AP"
SSID="${AIRPOC_AP_SSID:-AIRPOC}"

# Open (no-password) AP, 2.4 GHz for range, NAT + DHCP via ipv4.method=shared
# (gateway/host = 10.42.0.1). autoconnect off — the failover service owns activation.
if nmcli -t -f NAME connection show | grep -qx "$AP_CON"; then
  echo "profile $AP_CON already exists — leaving it as-is"
else
  sudo nmcli connection add type wifi ifname "$IFACE" con-name "$AP_CON" ssid "$SSID" \
       autoconnect no \
       802-11-wireless.mode ap 802-11-wireless.band bg \
       ipv4.method shared ipv6.method ignore
  # OPEN network — explicitly no security (as requested: no password)
  sudo nmcli connection modify "$AP_CON" 802-11-wireless-security.key-mgmt none 2>/dev/null || true
  echo "created open AP profile $AP_CON (ssid: $SSID)"
fi

sudo install -m 0755 "$DIR/airpoc-autoap.sh" /usr/local/bin/airpoc-autoap.sh
sudo cp "$DIR/airpoc-autoap.service" /etc/systemd/system/airpoc-autoap.service
sudo systemctl daemon-reload
sudo systemctl enable --now airpoc-autoap

echo
echo "WiFi failover installed."
echo "  bench : joins your home WiFi as today (nothing changes on the LAN)."
echo "  field : no home WiFi -> raises open '$SSID' -> phone joins -> http://10.42.0.1:8088/"
