"""IMX296 live quality preview (Orin Nano) — v4l2 AE + light ISP.

Uses the production nv_imx296 driver's real v4l2 controls (exposure in us,
gain 0-480) for auto-exposure — no i2c hack. Format is mono Y10.

Pipeline: Y10 (>>6) -> AE meters mean -> 3x3 median -> black-level + adaptive
white tone map -> gamma -> 8-bit -> MJPEG.
AE drives EXPOSURE first (low noise), gain only when exposure is maxed.

Run:  python3 imx296_preview.py        ->  http://<jetson-ip>:8091
"""
from __future__ import annotations
import argparse, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import cv2
import numpy as np

W, H = 1456, 1088
FRAME_BYTES = W * H * 2
PORT = 8091
PREVIEW_W = 900
JPEG_QUALITY = 72
ENGINE_MAX_FPS = 30
GAMMA = 0.85
TARGET = 450.0                  # AE target mean of the 10-bit luma
DEV = "/dev/video0"
EXP_MIN, EXP_MAX = 29, 16000    # us (driver exposure control range)
GAIN_MIN, GAIN_MAX = 0, 480     # 0.1 dB steps
BLACK = 60.0                    # sensor black level (BLKLEVEL 0x3c)

_GAMMA_LUT = np.array([((i / 255.0) ** (1.0 / GAMMA)) * 255.0
                       for i in range(256)], dtype=np.uint8)
_ROW_KER = np.ones(31) / 31.0          # vertical low-pass for row-noise removal


def _set_ctrl(exp, gain):
    subprocess.run(["v4l2-ctl", "-d", DEV, "--set-ctrl", f"exposure={int(exp)},gain={int(gain)}"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Cap:
    def __init__(self):
        self._open()

    def _open(self):
        self.proc = subprocess.Popen(
            ["v4l2-ctl", "-d", DEV, "--stream-mmap", "--stream-count=0", "--stream-to=-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)

    def grab(self):
        buf = bytearray()
        while len(buf) < FRAME_BYTES:
            c = self.proc.stdout.read(FRAME_BYTES - len(buf))
            if not c:
                try: self.proc.terminate()
                except Exception: pass
                time.sleep(0.2); self._open(); return None
            buf += c
        return bytes(buf)   # raw — keep the reader light so it drains the pipe fast


class Engine:
    def __init__(self):
        self.cap = Cap()
        self.exp = 8000
        self.gain = 0
        _set_ctrl(self.exp, self.gain)
        self._lock = threading.Lock(); self._jpeg = b""; self._running = True
        self._fps = 0.0; self._tl = time.time(); self._mean = 0.0; self._frame_n = 0
        self._latest = None; self._llock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._loop, daemon=True).start()

    def _reader(self):
        # Drain the capture pipe continuously so v4l2-ctl never blocks on a full
        # buffer — that backpressure is what overwrote/tore frames. Keep latest.
        while self._running:
            f10 = self.cap.grab()
            if f10 is not None:
                with self._llock:
                    self._latest = f10

    def _ae(self, mean10):
        """Exposure first for low noise; gain only when exposure is maxed."""
        self._mean = mean10
        err = TARGET - mean10
        if abs(err) < 30:
            return
        if err > 0:                                  # too dark
            if self.exp < EXP_MAX:
                self.exp = min(EXP_MAX, int(self.exp * 1.4) + 60)
            elif self.gain < GAIN_MAX:
                self.gain = min(GAIN_MAX, self.gain + 30)
        else:                                        # too bright
            if self.gain > GAIN_MIN:
                self.gain = max(GAIN_MIN, self.gain - 30)
            elif self.exp > EXP_MIN:
                self.exp = max(EXP_MIN, int(self.exp * 0.7))
        _set_ctrl(self.exp, self.gain)

    def _loop(self):
        period = 1.0 / ENGINE_MAX_FPS
        while self._running:
            t0 = time.time()
            with self._llock:
                raw = self._latest; self._latest = None
            if raw is None:
                time.sleep(0.004); continue
            f10 = (np.frombuffer(raw, dtype="<u2").reshape(H, W) >> 6)
            now = time.time(); dt = now - self._tl; self._tl = now
            if dt > 0:
                inst = 1.0 / dt
                self._fps = 0.85 * self._fps + 0.15 * inst if self._fps else inst
            self._frame_n += 1
            if self._frame_n % 4 == 0:
                self._ae(float(f10[::8, ::8].mean()))
            # --- ISP: median -> row-noise de-band -> black-level + adaptive white -> gamma ---
            den = cv2.medianBlur(f10.astype(np.uint16), 3).astype(np.float32)
            # horizontal row-noise correction: subtract the high-freq part of the
            # per-row median (sampled columns for speed) -> kills IMX296 banding.
            rowmed = np.median(den[:, ::4], axis=1)
            rowsm = np.convolve(rowmed, _ROW_KER, mode="same")
            den = den - (rowmed - rowsm)[:, None]
            white = float(np.percentile(den[::4, ::4], 99.5))
            if white <= BLACK + 1:
                white = BLACK + 1
            y8 = np.clip((den - BLACK) * (255.0 / (white - BLACK)), 0, 255).astype(np.uint8)
            y8 = cv2.LUT(y8, _GAMMA_LUT)
            sc = PREVIEW_W / W
            disp = cv2.cvtColor(cv2.resize(y8, (int(W * sc), int(H * sc))), cv2.COLOR_GRAY2BGR)
            lines = [f"IMX296 Y10  {self._fps:.0f} fps  mean={self._mean:.0f}/1023",
                     f"AE(v4l2): exp={self.exp}us  gain={self.gain}/480"]
            yt = int(H * sc) - 12
            for ln in reversed(lines):
                cv2.putText(disp, ln, (10, yt), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 3, cv2.LINE_AA)
                cv2.putText(disp, ln, (10, yt), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 1, cv2.LINE_AA)
                yt -= 26
            ok, b = cv2.imencode(".jpg", disp, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            if ok:
                with self._lock:
                    self._jpeg = b.tobytes()
            slack = period - (time.time() - t0)
            if slack > 0:
                time.sleep(slack)

    def get(self):
        with self._lock:
            return self._jpeg


ENGINE = None
HTML = (b"<!DOCTYPE html><html><head><title>IMX296 Live</title>"
        b"<style>body{background:#111;margin:0;display:flex;justify-content:center;"
        b"align-items:center;height:100vh}img{max-width:100vw;max-height:100vh}</style>"
        b"</head><body><img src='/stream'></body></html>")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/":
            self.send_response(200); self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(HTML))); self.end_headers()
            self.wfile.write(HTML)
        elif self.path == "/snap":
            j = ENGINE.get()
            self.send_response(200); self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(j))); self.end_headers()
            self.wfile.write(j)
        elif self.path == "/stream":
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.end_headers()
            try:
                while True:
                    j = ENGINE.get()
                    if j:
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                         + str(len(j)).encode() + b"\r\n\r\n" + j + b"\r\n")
                    time.sleep(0.03)
            except (BrokenPipeError, ConnectionResetError): pass
        else:
            self.send_response(404); self.end_headers()


def main():
    global ENGINE
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    ENGINE = Engine()
    print(f"\n  IMX296 live preview (v4l2 AE): http://0.0.0.0:{args.port}/\n", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
