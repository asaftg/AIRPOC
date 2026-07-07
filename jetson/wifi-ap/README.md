# AIRPOC WiFi failover — point-to-point AP in the field

One WiFi radio, used two ways automatically:

- **Bench** — joins your home WiFi (`Ruf USA`) exactly as today. Reach the box at
  `http://orin-nano.lan:8088/`. Nothing on the LAN changes.
- **Field** — no home WiFi in range, so the Jetson raises its **own open access point**
  `AIRPOC` (no password). Join it from your phone and go to **`http://10.42.0.1:8088/`**.
  All EO video + radar + console stream over that direct link — **no cellular, no router,
  no internet needed**. Your phone keeps its own cellular data for the outside world.

```
  bench:   phone ─┐                          field:   phone ──▶ AIRPOC (open)
  laptop ─┐       │  Ruf USA  ┌── Jetson               10.42.0.1:8088
  tablet ─┴──────▶│  (router) │   (client)             (Jetson is the AP)
```

## How it decides (`airpoc-autoap.sh`, a root service)

| situation | action |
|---|---|
| connected to home WiFi | **do nothing** — no scans, so a live stream is never disturbed |
| no WiFi, home network in range | (re)join home |
| no WiFi, nothing known in range | raise the open `AIRPOC` AP |
| in AP mode, **no** phone joined, home back in range | drop AP, rejoin home |
| in AP mode, **a** phone is joined | stay — never cut off the operator |

## Install (once, on the Jetson)

```
cd ~/AIRPOC && git pull
cd jetson/wifi-ap && ./install.sh
```

Safe to run over the home WiFi: the AP profile is `autoconnect=off` and the service
prefers home whenever it's in range, so your current connection is not dropped.

## Notes

- **Open network, on purpose** — you asked for no password. Anyone within WiFi range can
  join `AIRPOC` and reach the control page (`:8088`) and console (`:8080`), which can fire
  the illuminator. Fine for an isolated field site; add a passphrase later by setting
  `802-11-wireless-security` on the `AIRPOC-AP` profile if you ever want it.
- 5 GHz (band a, ch 36) for throughput — 2.4 GHz was slow/congested and Realtek AP mode is
  weak there. Trade-off is shorter range; for 2.4 GHz range fallback set `AIRPOC_AP_BAND=bg
  AIRPOC_AP_CHAN=6` before install (or `nmcli connection modify AIRPOC-AP 802-11-wireless.band bg`).
  Fixed host/gateway `10.42.0.1`, DHCP handed to clients.
- Change the SSID: `AIRPOC_AP_SSID=whatever ./install.sh` (before first install), or
  `nmcli connection modify AIRPOC-AP wifi.ssid whatever`.
