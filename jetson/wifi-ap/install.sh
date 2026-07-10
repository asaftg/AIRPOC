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
#
# IMPORTANT: for a truly OPEN network the profile must carry NO 802-11-wireless-security
# setting at all. In NetworkManager key-mgmt=none means *WEP* (it will demand a wep-key),
# not open. So we (re)create the profile clean, without any security setting.
if nmcli -t -f NAME connection show | grep -qx "$AP_CON"; then
  sudo nmcli connection delete "$AP_CON" >/dev/null
fi
# 2.4 GHz (band bg, ch 6) is the DEFAULT — hard lesson 2026-07-09: a 5 GHz ch36 AP is
# INVISIBLE to most phones (they don't scan/allow that channel) and depends on the regdomain
# being set, so the AP "didn't work" in the field even though hostapd was up. 2.4 GHz is seen
# by every device, has better range/penetration, and works regardless of regdomain — the
# right trade for a control link. Override with AIRPOC_AP_BAND=a / AIRPOC_AP_CHAN=36 for 5 GHz
# throughput on known-good client hardware in line-of-sight.
AP_BAND="${AIRPOC_AP_BAND:-bg}"; AP_CHAN="${AIRPOC_AP_CHAN:-6}"
sudo nmcli connection add type wifi ifname "$IFACE" con-name "$AP_CON" ssid "$SSID" \
     autoconnect no \
     802-11-wireless.mode ap 802-11-wireless.band "$AP_BAND" 802-11-wireless.channel "$AP_CHAN" \
     ipv4.method shared ipv6.method ignore
echo "created open AP profile $AP_CON (ssid: $SSID, band $AP_BAND ch $AP_CHAN)"

# shared state dir: the launcher (runs as $WIFI_USER) writes the desired mode here
# (auto|ap|home) and the failover daemon publishes live status back.
WIFI_USER="${AIRPOC_LAUNCHER_USER:-asaftg}"
sudo install -d -o "$WIFI_USER" -g "$WIFI_USER" /var/lib/airpoc
if [ ! -f /var/lib/airpoc/wifi-mode ]; then echo auto | sudo tee /var/lib/airpoc/wifi-mode >/dev/null; fi
sudo chown "$WIFI_USER":"$WIFI_USER" /var/lib/airpoc/wifi-mode

# captive-portal DNS: point OS connectivity-check domains at the AP so phones treat it
# as a real (internet-having) network and don't shove traffic to cellular.
sudo install -d /etc/NetworkManager/dnsmasq-shared.d
sudo install -m 0644 "$DIR/dnsmasq-shared.d/airpoc-captive.conf" /etc/NetworkManager/dnsmasq-shared.d/airpoc-captive.conf

sudo install -m 0755 "$DIR/airpoc-autoap.sh" /usr/local/bin/airpoc-autoap.sh
sudo cp "$DIR/airpoc-autoap.service" /etc/systemd/system/airpoc-autoap.service
sudo systemctl daemon-reload
sudo systemctl enable airpoc-autoap
sudo systemctl restart airpoc-autoap   # restart (not just enable) so a re-install loads new code

echo
echo "WiFi failover installed."
echo "  bench : joins your home WiFi as today (nothing changes on the LAN)."
echo "  field : no home WiFi -> raises open '$SSID' -> phone joins -> http://10.42.0.1:8088/"
