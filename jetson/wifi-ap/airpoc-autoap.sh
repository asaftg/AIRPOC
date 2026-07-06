#!/bin/bash
# AIRPOC WiFi failover. One radio, so it is client XOR access-point:
#   - connected to a known (home) WiFi  -> stay put, DO NOTHING (no scans, so a live
#     video stream over that link is never disturbed).
#   - no WiFi connection and no known network in range -> raise the open AIRPOC AP so a
#     phone can join point-to-point in the field (10.42.0.1).
#   - in AP mode with NO phone joined -> once every AP_PROBE_EVERY seconds, briefly drop
#     the AP to scan (a radio can't scan while it's an AP); if home WiFi is back, rejoin
#     it, else re-raise the AP. If a phone IS joined (operator working), never disturb it.
#
# Runs as root under systemd.
IFACE="${AIRPOC_WIFI_IFACE:-wlP1p1s0}"
AP_CON="AIRPOC-AP"
POLL="${AIRPOC_AUTOAP_POLL:-20}"                 # seconds between checks
AP_PROBE_EVERY="${AIRPOC_AP_PROBE_EVERY:-120}"   # seconds between home-probes while in AP

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
home_in_range() {   # rescan (radio must be in managed/idle mode) -> 0 if a saved net is visible
  nmcli dev wifi rescan ifname "$IFACE" 2>/dev/null || true
  sleep 3
  local vis con ssid
  vis=$(nmcli -t -f SSID dev wifi list ifname "$IFACE" 2>/dev/null)
  while read -r con; do
    [ -z "$con" ] && continue
    ssid=$(nmcli -t -g 802-11-wireless.ssid connection show "$con" 2>/dev/null); ssid="${ssid:-$con}"
    grep -qxF "$ssid" <<<"$vis" && return 0
  done < <(known_cons)
  return 1
}
join_home() { local first; first=$(known_cons | head -1); [ -n "$first" ] && nmcli connection up "$first" 2>/dev/null; }

now() { date +%s; }
last_probe=0

tick() {
  local act; act=$(active_con)

  if [ "$act" = "$AP_CON" ]; then
    # In AP mode. Never disturb a connected operator.
    [ "$(ap_clients)" -gt 0 ] && return
    # No phone joined: occasionally drop the AP to look for home (can't scan as an AP).
    local t; t=$(now)
    if [ $(( t - last_probe )) -ge "$AP_PROBE_EVERY" ]; then
      last_probe=$t
      log "AP idle — briefly dropping to probe for home WiFi"
      nmcli connection down "$AP_CON" 2>/dev/null
      if home_in_range; then
        log "home WiFi is back -> rejoining"
        join_home
      else
        log "still no home -> re-raising AP"
        nmcli connection up "$AP_CON" 2>/dev/null
      fi
    fi
    return
  fi

  if [ -n "$act" ]; then
    return   # connected as a client -> idle, do not disturb the link
  fi

  # No wifi connection at all. Prefer a known network; only raise the AP if none is near.
  if home_in_range; then
    log "no connection but home WiFi in range -> joining"
    join_home
  else
    log "no known WiFi in range -> raising open AP $AP_CON"
    nmcli connection up "$AP_CON" 2>/dev/null
    last_probe=$(now)   # don't probe again until AP_PROBE_EVERY has passed
  fi
}

log "started (iface=$IFACE ap=$AP_CON poll=${POLL}s probe=${AP_PROBE_EVERY}s)"
while true; do tick; sleep "$POLL"; done
