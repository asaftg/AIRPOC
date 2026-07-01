"""IMX296 focus assist — web edition (Orin Nano), stdlib-only.

Adapted from the seeker ground-bench eo_focus_web.py (read-only source).
Same Tenengrad + Laplacian sharpness math, served as MJPEG over HTTP so you
focus the M12 lens from a browser with no GUI on the Jetson and no extra deps
(uses http.server; only needs cv2 + numpy, which ship with JetPack).

Capture: raw Y10 frames from /dev/video0 via a v4l2-ctl stdout pipe (~52 fps).
IMX296 Y10 is LEFT-JUSTIFIED in the 16-bit word -> top 8 bits (>>8) for preview.
Nothing else may hold the camera (no Argus/other v4l2 reader).

Run on the Jetson:  python3 imx296_focus_web.py
Open  http://<jetson-ip>:8090  and turn the focus ring until Tenengrad /
Laplacian read ~100% of best.  Visit /reset to rescale the peak.
"""
from __future__ import annotations
import argparse, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import cv2
import numpy as np

def _probe_fmt(dev="/dev/video0"):
    """Probe the driver's real geometry. The sensor is ROI-cropped to 1440 so its
    Y10 line is 2880 B (64-byte aligned). Hardcoding 1456 reshapes the 2880-byte
    line as 2912 -> every row drifts +16 px -> diagonal shear (the 'lines')."""
    try:
        o = subprocess.run(["v4l2-ctl", "-d", dev, "--get-fmt-video"],
                           capture_output=True, text=True).stdout
        w = h = bpl = si = 0
        for ln in o.splitlines():
            if "Width/Height" in ln:
                a, b = ln.split(":")[1].split("/"); w, h = int(a), int(b)
            elif "Bytes per Line" in ln: bpl = int(ln.split(":")[1])
            elif "Size Image" in ln:     si = int(ln.split(":")[1])
        if w and h and bpl:
            return w, h, bpl, (si or bpl * h)
    except Exception:
        pass
    return 1440, 1088, 2880, 2880 * 1088

W, H, BPL, FRAME_BYTES = _probe_fmt()
PORT = 8090
ROI_FRAC = 0.3
PREVIEW_W = 640          # smaller preview -> much less data over WiFi
JPEG_QUALITY = 55        # lighter JPEGs -> smoother browser fps
ENGINE_MAX_FPS = 30      # cap processing so it doesn't starve the HTTP stream


def _tenengrad(g):
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    return float((gx * gx + gy * gy).mean())


def _laplacian_var(g):
    return float(cv2.Laplacian(g, cv2.CV_32F, ksize=3).var())


class V4L2Y10Capture:
    def __init__(self, device="/dev/video0"):
        self.device = device
        self.proc = None
        self._open()

    def _open(self):
        self.proc = subprocess.Popen(
            ["v4l2-ctl", "-d", self.device, "--stream-mmap",
             "--stream-count=0", "--stream-to=-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)

    def grab(self):
        buf = bytearray()
        while len(buf) < FRAME_BYTES:
            chunk = self.proc.stdout.read(FRAME_BYTES - len(buf))
            if not chunk:
                try: self.proc.terminate()
                except Exception: pass
                time.sleep(0.2); self._open(); return None
            buf += chunk
        f16 = np.frombuffer(bytes(buf), dtype="<u2").reshape(H, BPL // 2)[:, :W]
        return (f16 >> 8).astype(np.uint8)      # left-justified 10-bit -> 8-bit

    def stop(self):
        try: self.proc.terminate()
        except Exception: pass


class FocusEngine:
    def __init__(self, device, roi_frac):
        self.cap = V4L2Y10Capture(device)
        self.roi_frac = roi_frac
        self.best_t = self.best_l = 0.0
        self._fps = 0.0; self._tl = time.time()
        self._lock = threading.Lock(); self._jpeg = b""; self._running = True
        threading.Thread(target=self._loop, daemon=True).start()

    def reset_best(self):
        self.best_t = self.best_l = 0.0

    def _loop(self):
        period = 1.0 / ENGINE_MAX_FPS
        while self._running:
            t0 = time.time()
            g = self.cap.grab()
            if g is None:
                continue
            now = time.time(); dt = now - self._tl; self._tl = now
            if dt > 0:
                inst = 1.0 / dt
                self._fps = 0.85 * self._fps + 0.15 * inst if self._fps else inst
            h, w = g.shape
            rw = max(8, int(w * self.roi_frac)); rh = max(8, int(h * self.roi_frac))
            x0 = (w - rw) // 2; y0 = (h - rh) // 2
            roi = g[y0:y0 + rh, x0:x0 + rw]
            ten = _tenengrad(roi); lap = _laplacian_var(roi)
            self.best_t = max(self.best_t, ten); self.best_l = max(self.best_l, lap)
            rt = ten / self.best_t * 100 if self.best_t else 0
            rl = lap / self.best_l * 100 if self.best_l else 0
            sc = PREVIEW_W / w if w > PREVIEW_W else 1.0
            disp = cv2.cvtColor(cv2.resize(g, (int(w * sc), int(h * sc))),
                                cv2.COLOR_GRAY2BGR)
            cv2.rectangle(disp, (int(x0 * sc), int(y0 * sc)),
                          (int((x0 + rw) * sc), int((y0 + rh) * sc)), (0, 255, 0), 2)
            lines = [
                f"IMX296 {w}x{h} @ {self._fps:.1f} fps",
                f"Tenengrad: {ten:.0f}  ({rt:.0f}% of best)",
                f"Laplacian: {lap:.0f}  ({rl:.0f}% of best)",
                "Turn focus ring until both read ~100%",
            ]
            yt = int(h * sc) - 12
            for ln in reversed(lines):
                cv2.putText(disp, ln, (10, yt), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 0), 3, cv2.LINE_AA)
                cv2.putText(disp, ln, (10, yt), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 1, cv2.LINE_AA)
                yt -= 30
            ok, b = cv2.imencode(".jpg", disp, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            if ok:
                with self._lock:
                    self._jpeg = b.tobytes()
            slack = period - (time.time() - t0)
            if slack > 0:
                time.sleep(slack)

    def get_jpeg(self):
        with self._lock:
            return self._jpeg


ENGINE = None
HTML = (b"<!DOCTYPE html><html><head><title>IMX296 Focus Assist</title>"
        b"<style>body{background:#111;margin:0;display:flex;justify-content:center;"
        b"align-items:center;height:100vh}img{max-width:100vw;max-height:100vh}</style>"
        b"</head><body><img src='/stream'></body></html>")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(HTML)))
            self.end_headers()
            self.wfile.write(HTML)
        elif self.path == "/reset":
            ENGINE.reset_best()
            self.send_response(200); self.end_headers(); self.wfile.write(b"reset")
        elif self.path == "/stream":
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.end_headers()
            try:
                while True:
                    j = ENGINE.get_jpeg()
                    if j:
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                         + str(len(j)).encode() + b"\r\n\r\n" + j + b"\r\n")
                    time.sleep(0.03)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_response(404); self.end_headers()


def main():
    global ENGINE
    ap = argparse.ArgumentParser(description="IMX296 focus assist (web, stdlib)")
    ap.add_argument("--device", default="/dev/video0")
    ap.add_argument("--roi-frac", type=float, default=ROI_FRAC)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    ENGINE = FocusEngine(args.device, args.roi_frac)
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"\n  IMX296 focus assist: http://0.0.0.0:{args.port}/  (/reset to rescale)\n", flush=True)
    try:
        srv.serve_forever()
    finally:
        ENGINE.cap.stop()


if __name__ == "__main__":
    main()
