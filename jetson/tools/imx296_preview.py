"""IMX296 quality preview (Orin Nano) — i2c auto-exposure + light ISP.

The INNO-MAKER prebuilt driver's v4l2 exposure/gain controls are DEAD: set
values read back correctly but never reach the sensor, so the image is stuck
dark. Proven by a direct i2c write -- SHS1=8 took the same scene from mean 59
to 847. So this tool runs AE by writing the sensor registers directly over
i2c (bus auto-detected from sysfs), bypassing the broken control path:
    SHS1 (0x308d, 24-bit) = VMAX - exposure_lines   (smaller SHS1 = brighter)
    GAIN (0x3204, 16-bit, 0..480, 0.1 dB/step)
This is a focus/preview TOOL (Python is fine). The shipping exposure control
belongs in the C driver's s_ctrl.

Pipeline: Y10 (left-justified, >>6) -> AE meters mean -> 3x3 median ->
black-level tone map -> gamma 0.85 -> 8-bit -> MJPEG.
AE drives exposure first (low noise), raising gain only when exposure is maxed.

Run:  python3 imx296_preview.py        ->  http://<jetson-ip>:8091
"""
from __future__ import annotations
import argparse, glob, os, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import cv2
import numpy as np

W, H = 1456, 1088
FRAME_BYTES = W * H * 2
PORT = 8091
PREVIEW_W = 760
JPEG_QUALITY = 70
ENGINE_MAX_FPS = 30
GAMMA = 0.85
TARGET = 430.0                  # AE target mean of the 10-bit luma (~42%)
DEV = "/dev/video0"
ADDR = "0x1a"
SHS1_MIN, SHS1_MAX = 8, 1100    # smaller SHS1 = longer exposure = brighter
GAIN_MIN, GAIN_MAX = 0, 480
BLACK, WHITE = 60.0, 720.0      # tone map: BLACK->0, WHITE->255 (no auto-stretch)


def _i2c_bus():
    for p in glob.glob("/sys/bus/i2c/devices/*-001a"):
        b = os.path.basename(p).split("-")[0]
        if b.isdigit():
            return b
    return None


BUS = _i2c_bus()


def _i2c_w(reghi, reglo, data_bytes):
    if BUS is None:
        return
    n = len(data_bytes) + 2          # 2 register-address bytes + data
    subprocess.run(["i2ctransfer", "-f", "-y", BUS, f"w{n}@{ADDR}", reghi, reglo]
                   + [f"0x{b:02x}" for b in data_bytes],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _set_shs1(v):
    v = int(max(SHS1_MIN, min(SHS1_MAX, v)))
    _i2c_w("0x30", "0x8d", [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff])


def _set_gain(v):
    v = int(max(GAIN_MIN, min(GAIN_MAX, v)))
    _i2c_w("0x32", "0x04", [v & 0xff, (v >> 8) & 0xff])


_GAMMA_LUT = np.array([((i / 255.0) ** (1.0 / GAMMA)) * 255.0
                       for i in range(256)], dtype=np.uint8)


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
        return (np.frombuffer(bytes(buf), dtype="<u2").reshape(H, W) >> 6)  # 10-bit


class Engine:
    def __init__(self):
        self.cap = Cap()
        self.shs1 = 400
        self.gain = 40
        _set_gain(self.gain); _set_shs1(self.shs1)
        self._lock = threading.Lock(); self._jpeg = b""; self._running = True
        self._fps = 0.0; self._tl = time.time(); self._mean = 0.0; self._frame_n = 0
        threading.Thread(target=self._loop, daemon=True).start()

    def _ae(self, mean10):
        """Exposure (SHS1) first for low noise; gain only when exposure maxed."""
        self._mean = mean10
        err = TARGET - mean10
        if abs(err) < 30:
            return
        if err > 0:                                    # too dark -> longer exposure
            if self.shs1 > SHS1_MIN:
                self.shs1 = max(SHS1_MIN, self.shs1 - max(4, int(self.shs1 * 0.25)))
                _set_shs1(self.shs1)
            elif self.gain < GAIN_MAX:
                self.gain = min(GAIN_MAX, self.gain + 24); _set_gain(self.gain)
        else:                                          # too bright
            if self.gain > GAIN_MIN:
                self.gain = max(GAIN_MIN, self.gain - 24); _set_gain(self.gain)
            elif self.shs1 < SHS1_MAX:
                self.shs1 = min(SHS1_MAX, self.shs1 + max(4, int(self.shs1 * 0.25)))
                _set_shs1(self.shs1)

    def _loop(self):
        period = 1.0 / ENGINE_MAX_FPS
        while self._running:
            t0 = time.time()
            f10 = self.cap.grab()
            if f10 is None:
                continue
            now = time.time(); dt = now - self._tl; self._tl = now
            if dt > 0:
                inst = 1.0 / dt
                self._fps = 0.85 * self._fps + 0.15 * inst if self._fps else inst
            self._frame_n += 1
            if self._frame_n % 4 == 0:
                self._ae(float(f10[::8, ::8].mean()))
            # --- light ISP: median denoise -> fixed tone map -> gamma ---
            den = cv2.medianBlur(f10.astype(np.uint16), 3)
            y8 = np.clip((den.astype(np.float32) - BLACK) * (255.0 / (WHITE - BLACK)), 0, 255).astype(np.uint8)
            y8 = cv2.LUT(y8, _GAMMA_LUT)
            sc = PREVIEW_W / W
            disp = cv2.cvtColor(cv2.resize(y8, (int(W * sc), int(H * sc))), cv2.COLOR_GRAY2BGR)
            lines = [f"IMX296 AE+ISP  {self._fps:.0f} fps  mean={self._mean:.0f}/1023",
                     f"exp(SHS1)={self.shs1}  gain={self.gain}/480  (i2c AE, low-gain)"]
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
HTML = (b"<!DOCTYPE html><html><head><title>IMX296 Quality Preview</title>"
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
    if BUS is None:
        print("WARNING: camera i2c bus (addr 0x1a) not found — AE disabled", flush=True)
    ENGINE = Engine()
    print(f"\n  IMX296 quality preview (i2c AE on bus {BUS}): http://0.0.0.0:{args.port}/\n", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
