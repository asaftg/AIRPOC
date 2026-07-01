# EO Focus Tool

`eo/tools/imx296_focus_web.py` — a browser focus-assist for setting the M12 lens.
It streams the live image with two sharpness metrics so you can peak focus by eye.

## Run
```bash
bash eo/tools/focus.sh        # http://<ip>:8090   (or 192.168.55.1 over USB-C)
```

## Use
1. Point the camera at a detailed, well-lit target with hard edges (not a blank wall).
2. Aim the green ROI box at that detail.
3. Turn the M12 lens barrel slowly; watch **Tenengrad** and **Laplacian** — both are
   shown with a "% of best". Turn until both peak (~100%).
4. Lock the lens set-screw. `http://<ip>:8090/reset` rescales the metric if it saturates.

> Pitfall: the tool probes the driver's real width from v4l2. If a tool hardcodes a
> stale width (e.g. 1456 when the sensor outputs 1440) it reshapes the line wrong and
> shears the image diagonally — a tool bug, not optics. Always read `bytesperline`.
