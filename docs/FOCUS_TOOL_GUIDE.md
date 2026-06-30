# IMX296 Focus Tool — Guide

Browser-based focus assist for the IMX296 on the Orin Nano. Live preview + two
sharpness metrics (**Tenengrad**, **Laplacian**) — you focus the M12 lens by
turning the ring until the numbers peak. Adapted from the seeker ground-bench
`eo_focus_web.py`.

## Start it
```bash
# from your laptop (one line):
ssh asaftg@orin-nano 'bash ~/focus.sh'      # or use the IP: asaftg@192.168.86.101
```
Then open in a browser:
```
http://orin-nano:8090            # or http://192.168.86.101:8090
```
`~/focus.sh` kills any running instance and relaunches. (If `orin-nano` doesn't
resolve, find the IP with `ssh asaftg@192.168.55.1 hostname -I`.)

## Focus
1. The **green box** is the focus region (center 30%). Aim it at something with detail/edges.
2. Turn the **M12 lens ring** slowly. **Tenengrad** and **Laplacian** rise as the image sharpens.
3. **Best focus = both metrics at ~100% of best.** Rock back and forth around the peak.
4. Overshot and want to rescale the peak? Visit `http://orin-nano:8090/reset`.

## Stop it
```bash
ssh asaftg@orin-nano 'pkill -f imx296_focus_web'
```

## Notes
- **Exclusive camera:** the tool holds `/dev/video0`. Stop it before any other capture (and vice-versa) or you'll get a busy/timeout.
- **Preview is 640 px / ~30 fps on purpose** — light enough for smooth WiFi. The sensor itself runs a true 60 fps; the throttle is only in this Python tool. Adjust `ENGINE_MAX_FPS` / `PREVIEW_W` in `~/imx296_focus_web.py` if you want.
- **Faster link:** if WiFi lags, view over the USB-C net at `http://192.168.55.1:8090` (when your machine is cabled to the Jetson).
- Files: `~/imx296_focus_web.py` + `~/focus.sh` (also in `AIRPOC/jetson/tools/`).
- Image is raw Y10 mono (no ISP yet) so it looks dark/grainy — that's the ISP/AE work, separate from focus.
