#!/bin/bash
# AIRPOC WiFi control + failover. One radio, so it is client XOR access-point. A mode
# file (written by the launcher page) picks the policy:
#   auto  - home WiFi if a known network is in range, else raise the open AIRPOC AP
#   ap    - PINNED access point: keep AIRPOC up, never fail back, never scan-drop (most
#           reliable for field work)
#   home  - PINNED client: stay on home WiFi, never raise the AP
# The daemon also publishes the live state to a status file the launcher reads.
#
# Runs as root under systemd.
IFACE="${AIRPOC_WIFI_IFACE:-wlP1p1s0}"
AP_CON="AIRPOC-AP"
COUNTRY="${AIRPOC_WIFI_COUNTRY:-US}"             # regdomain for the AP (override per deploy region)
POLL="${AIRPOC_AUTOAP_POLL:-3}"                  # seconds between checks (short = snappy mode switches)
AP_PROBE_EVERY="${AIRPOC_AP_PROBE_EVERY:-120}"   # seconds between home-probes while auto+AP
MODE_FILE="${AIRPOC_WIFI_MODE_FILE:-/var/lib/airpoc/wifi-mode}"
STATUS_FILE="${AIRPOC_WIFI_STATUS_FILE:-/var/lib/airpoc/wifi-status}"

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
home_in_range() {   # rescan (radio must be managed/idle) -> 0 if a saved net is visible
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

# A 5GHz AP can only beacon under a real regulatory domain. The board boots as world
# (country 00), which forbids 5GHz AP — hostapd then times out ("Hotspot creation took too
# long", supplicant-timeout) and no SSID appears. Pin the regdomain before every raise so
# the AP survives reboots and driver resets. (Home WiFi sometimes sets it via a country IE,
# which is why it "worked mid-day" then broke — don't rely on that.)
ensure_reg() {
  local c; c=$(iw reg get 2>/dev/null | awk '/^country/{print $2; exit}'); c="${c%:}"
  [ "$c" = "$COUNTRY" ] || { log "regdomain '$c' -> $COUNTRY (5GHz AP needs a real country)"; iw reg set "$COUNTRY" 2>/dev/null; sleep 1; }
}
raise_ap() { ensure_reg; nmcli connection up "$AP_CON" 2>/dev/null; }

# default AUTO: home WiFi when a known network is in range, else raise the AP. This is the
# intended field behaviour ("home==wifi, outside==AP") and keeps the Jetson able to reach
# the cloud (agents push, Jetson pulls). Only an explicit mode file pins home/ap.
read_mode() { local m; m=$(tr -d '[:space:]' < "$MODE_FILE" 2>/dev/null); case "$m" in ap|home|auto) echo "$m";; *) echo auto;; esac; }
now() { date +%s; }
last_probe=0

write_status() {   # $1 = selected mode
  local act ap net ip
  act=$(active_con)
  if [ "$act" = "$AP_CON" ]; then ap=true; net="AIRPOC"; ip="10.42.0.1"
  elif [ -n "$act" ]; then ap=false; net="$act"; ip=$(ip -4 addr show "$IFACE" 2>/dev/null | awk '/inet /{print $2; exit}'); ip="${ip%%/*}"
  else ap=false; net=""; ip=""; fi
  printf '{"mode":"%s","ap":%s,"net":"%s","ip":"%s","clients":%s}\n' \
    "$1" "$ap" "$net" "$ip" "$(ap_clients)" > "${STATUS_FILE}.tmp" 2>/dev/null && mv "${STATUS_FILE}.tmp" "$STATUS_FILE" 2>/dev/null
}

auto_tick() {   # $1 = current active connection
  local act="$1"
  if [ "$act" = "$AP_CON" ]; then
    [ "$(ap_clients)" -gt 0 ] && return              # operator connected -> never disturb
    local t; t=$(now)
    if [ $(( t - last_probe )) -ge "$AP_PROBE_EVERY" ]; then
      last_probe=$t
      log "auto: AP idle -> briefly probing for home WiFi"
      nmcli connection down "$AP_CON" 2>/dev/null
      if home_in_range; then log "home is back -> rejoining"; join_home
      else log "still no home -> re-raising AP"; raise_ap; fi
    fi
    return
  fi
  [ -n "$act" ] && return                            # connected as client -> idle
  # No connection yet. At boot the radio + NM autoconnect + the wifi scan are slow to settle,
  # so a single empty scan here makes it raise the AP even though home WiFi is right there —
  # that's why it came up on the AP instead of joining WiFi after a reboot. Try a few times
  # (each home_in_range does a rescan) and bail the moment NM has joined a client network.
  local tries=0
  while [ "$tries" -lt 3 ]; do
    [ -n "$(active_con)" ] && return                 # NM autoconnected home meanwhile
    home_in_range && { log "auto: home in range -> joining"; join_home; return; }
    tries=$((tries + 1))
  done
  log "auto: no known WiFi after 3 scans -> raising AP"; raise_ap; last_probe=$(now)
}

tick() {
  local mode act; mode=$(read_mode); act=$(active_con)
  case "$mode" in
    home)   # pinned client: drop AP, join home, never raise AP
      [ "$act" = "$AP_CON" ] && { log "mode=home -> leaving AP"; nmcli connection down "$AP_CON" 2>/dev/null; act=""; }
      [ -z "$act" ] && { home_in_range && join_home; } ;;
    ap)     # pinned AP: keep it up, no fail-back, no scan-drops
      [ "$act" = "$AP_CON" ] || { log "mode=ap -> raising AP"; raise_ap; } ;;
    *)      auto_tick "$act" ;;
  esac
  write_status "$mode"
}

log "started (iface=$IFACE ap=$AP_CON poll=${POLL}s probe=${AP_PROBE_EVERY}s mode-file=$MODE_FILE)"
while true; do tick; sleep "$POLL"; done
