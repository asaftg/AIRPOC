# EO Focus Assist

Focus assist is built into the EO preview (`eo/pipeline/`, the `eo_pipeline` server) —
open the page and click **focus**. It shows a center ROI box and a live sharpness value
with a "% of best" tracker so you can peak the M12 lens by eye.

## Run
```bash
cd eo/pipeline && ./eo_pipeline -d /dev/video0 -p 8091
# open http://<ip>:8091/  (or 192.168.55.1 over USB-C), click "focus"
```

## Use
1. Point the camera at a detailed, well-lit target with hard edges (not a blank wall).
2. Aim the green ROI box at that detail.
3. Click **focus**; turn the M12 lens barrel slowly and watch the **sharpness** climb
   toward its **peak %** (Tenengrad on the native 10-bit center ROI — correct at any
   zoom, since it reads the raw frame, not the display).
4. Lock the lens set-screw at the peak. Toggling focus off/on rescales the peak.

> Pitfall: the sharpness metric reads the driver's real width from V4L2 (`bytesperline`).
> A stale hardcoded width (e.g. 1456 when the sensor outputs 1440) reshapes the line and
> shears the image diagonally — a stride bug, not optics. Always read `bytesperline`.
