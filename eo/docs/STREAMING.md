# EO Streaming, codecs & the datalink

How the EO feed is delivered, and the plan for the RF datalink.

## Two consumers, two needs
- **Detector** (on-device): consumes the **full-native 1440×1088** frame directly from
  `libeo` — no encode, no downscale, ever.
- **Operator display**: an encoded stream sized for the link. The operator picks the
  size (`/ctl?res=`) and rate (`/ctl?fps=`); shrinking it never touches the detector.

## Platform fact: no hardware video encoder
> The Orin Nano Super has **no NVENC** (fused off) and its NVJPG engine is decode-only.
> So every encoded stream is **software** on the A78 cores. GStreamer `v4l2src` also
> can't negotiate Y10 — feed via `appsrc` from a V4L2 reader.

## Codec choice: MJPEG (LAN) vs H.264 (radio)

| | MJPEG | H.264 (x264 ultrafast) |
|---|---|---|
| Compression | intra-only (each frame alone) | intra **+ inter** (predicts across frames) |
| CPU | cheap | pricier (motion estimation) |
| Bandwidth | **~5–10× fatter** | skinny |
| Use | **LAN / bench** (current `/stream`) | **RF datalink** (future) |

The old "x264 ≈ 6 fps, not viable" note was for full-native 1.6 MP — which nobody
streams over a radio. **At the display sizes we actually send** (480p/720p) software
x264 ultrafast is cheap: ~0.3–0.5 core, ~1–4 Mbps. Resolution is the lever, not the
codec. MJPEG is only "free" on CPU; at any watchable size it's 30–60 Mbps, which the
radio can't carry.

## Current: MJPEG over HTTP (LAN / bench)
`eo_pipeline` serves `multipart/x-mixed-replace` MJPEG on `:8091` at the selected 4:3
size. Best over the **USB-C link (`192.168.55.1`)** — wired, sub-ms latency. Fine on
LAN/WiFi for bench work; **not** the transport for the RF link.

## FUTURE WORK: H.264/RTSP over the SIYI datalink (when the HM30/MK15 arrives)

Target link: **SIYI HM30** (or MK15E — same air unit + a handheld ground controller).
1 km LOS has huge margin (rated 15–30 km). Architecture (verified from SIYI docs):

- The **air unit does not encode** — it's an IP relay. It pulls an **RTSP/H.265 stream
  over Ethernet** on the `192.168.144.x` subnet and puts it on the radio; the **ground
  unit** hands H.264/H.265 to QGC / our GUI to decode. Bitrates: 720p30 ≈ 1.6 Mbps,
  1080p30 ≈ 2.3 Mbps H.265. Latency: 720p30 ≈ 130–180 ms, 1080p30 ≈ 180–250 ms.

Two ways to feed it (the encoder is *before* the air unit):

| Path | fps | Orin CPU | Cost |
|---|---|---|---|
| **Orin software x264 → RTSP** (e.g. MediaMTX) on `192.168.144.x:8554` | up to 720p60 | ~1.5–2 cores @720p60 (much less at 480p/30) | $0 |
| **SIYI HDMI→Ethernet converter** (SY029, HW H.265) | **30 fps only** | **zero** | ~$170 |

Plan:
1. Keep the two-tap split — detector stays on full native; only the display encodes.
2. Add an **H.264/RTSP output** to the module (software x264 ultrafast/zerolatency),
   driven by the **same `res`/`fps` knobs**. Default a low display size (≤720p) so it's
   <0.5 core and fits the link with margin.
3. Detections ride the **telemetry/MAVLink** channel (kbps); the **ground** GUI
   composites video + overlays on the operator's laptop.
4. The 30 fps-capped SY029 converter is the zero-CPU option **for night** (where fps is
   already low for exposure) — its ceiling stops mattering exactly when we're below it.
5. Profile x264 alongside the detector/tracking on the 6 cores; drop resolution before
   dropping fps if cores get tight. Run MAXN-Super + `jetson_clocks` when profiling.

The image processing before the detector is in [IMAGE_PIPELINE](IMAGE_PIPELINE.md); the
module/preview and the control contract are in
[../pipeline/README.md](../pipeline/README.md) and
[../pipeline/INTEGRATION.md](../pipeline/INTEGRATION.md).
