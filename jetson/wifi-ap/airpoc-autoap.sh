#!/bin/bash
# AIRPOC WiFi failover. One radio, so it is client XOR access-point:
#   - connected to a known (home) WiFi  -> stay put, DO NOTHING (no scans, so a live
#     video stream over that link is never disturbed).
#   - no WiFi connection and no known network in range -> raise the open AIRPOC AP so a
#     phone can join point-to-point in the field (10.42.0.1).
#   - in AP mode with NO phone joined and home WiFi back in range -> drop the AP and
#     rejoin home. If a phone IS joined (operator working), never disrupt it.
#
# Runs as root under systemd. Poll interval below.
IFACE="${AIRPOC_WIFI_IFACE:-wlP1p1s0}"
AP_CON="AIRPOC-AP"
POLL="${AIRPOC_AUTOAP_POLL:-20}"

log() { echo "autoap: $*"; }

known_cons() {   # names of saved WiFi client profiles (everything except our AP)
  nmcli -t -f NAME,TYPE connection show | awk -F: '$2=="802-11-wireless"{print $1}' | grep -vx "$AP_CON"
}
active_con() {   # connection currently active on the wifi iface (empty if none)
  nmcli -t -f NAME,DEVICE connection show --active | awk -F: -v i="$IFACE" '$2==i{print $1}'
}
ap_clients() {   # how many phones are joined to our AP
  iw dev "$IFACE" station dump 2>/dev/null | grep -c "^Station"
}
home_in_range() {   # rescan and return 0 if any saved home network is visible
  nmcli dev wifi rescan ifname "$IFACE" 2>/dev/null || true
  sleep 3
  local vis; vis=$(nmcli -t -f SSID dev wifi list ifname "$IFACE" 2>/dev/null)
  local con ssid
  while read -r con; do
    [ -z "$con" ] && continue
    ssid=$(nmcli -t -g 802-11-wireless.ssid connection show "$con" 2>/dev/null)
    ssid="${ssid:-$con}"
    grep -qxF "$ssid" <<<"$vis" && return 0
  done < <(known_cons)
  return 1
}

tick() {
  local act; act=$(active_con)

  if [ "$act" = "$AP_CON" ]; then
    # In AP mode. Only fall back to home if no operator phone is connected.
    if [ "$(ap_clients)" -eq 0 ] && home_in_range; then
      log "home WiFi in range and no client joined -> leaving AP for client"
      nmcli connection down "$AP_CON" 2>/dev/null
      # nudge the first known profile; the rest is NM autoconnect
      local first; first=$(known_cons | head -1)
      [ -n "$first" ] && nmcli connection up "$first" 2>/dev/null
    fi
    return
  fi

  if [ -n "$act" ]; then
    return   # connected as a client -> idle, do not disturb the link
  fi

  # No wifi connection. Give a known network priority; only raise the AP if none is near.
  if home_in_range; then
    local first; first=$(known_cons | head -1)
    log "no connection but home WiFi in range -> (re)joining $first"
    [ -n "$first" ] && nmcli connection up "$first" 2>/dev/null
  else
    log "no known WiFi in range -> raising open AP $AP_CON"
    nmcli connection up "$AP_CON" 2>/dev/null
  fi
}

log "started (iface=$IFACE ap=$AP_CON poll=${POLL}s)"
while true; do tick; sleep "$POLL"; done
