"""IMX296 live quality preview (Orin Nano) — V4L2 mmap capture, AE + de-band ISP.

Capture is **proper V4L2 mmap** (ctypes ioctl: REQBUFS/QUERYBUF/QBUF/DQBUF) — each
DQBUF hands over one *complete* frame buffer, so there is no byte stream to
desync and no torn frames (the previous v4l2-ctl pipe could write a short frame
on overrun and permanently misalign a byte reader).

A reader thread DQBUFs continuously (copy + requeue, keeping the driver's buffer
ring fed); the ISP runs on the latest frame in a second thread.

AE writes the sensor SHS1/gain over i2c (no 2nd /dev/video0 handle -> no S_CTRL
glitch). The driver's v4l2 controls remain the production interface.

ISP: Y10(>>6) -> 3x3 median -> row-noise de-band -> black-level + adaptive-white
tone map -> gamma -> 8-bit -> MJPEG.  Run: python3 imx296_preview.py -> :8091
"""
from __future__ import annotations
import argparse, ctypes, fcntl, glob, mmap, os, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import cv2
import numpy as np

W, H = 1456, 1088
FRAME_BYTES = W * H * 2
PORT = 8091
PREVIEW_W = 900
JPEG_QUALITY = 92
ENGINE_MAX_FPS = 30
GAMMA = 0.85
TARGET = 450.0
DEV = "/dev/video0"
ADDR = "0x1a"
SHS1_MIN, SHS1_MAX = 8, 1100
GAIN_MIN, GAIN_MAX = 0, 480
BLACK = 60.0

_GAMMA_LUT = np.array([((i / 255.0) ** (1.0 / GAMMA)) * 255.0
                       for i in range(256)], dtype=np.uint8)
_ROW_KER = np.ones(31) / 31.0

# ── i2c AE (write sensor SHS1/gain directly; no /dev/video0 open) ──────────
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
    n = len(data_bytes) + 2
    subprocess.run(["i2ctransfer", "-f", "-y", BUS, f"w{n}@{ADDR}", reghi, reglo]
                   + [f"0x{b:02x}" for b in data_bytes],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def _set_shs1(v):
    v = int(max(SHS1_MIN, min(SHS1_MAX, v)))
    _i2c_w("0x30", "0x8d", [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff])

def _set_gain(v):
    v = int(max(GAIN_MIN, min(GAIN_MAX, v)))
    _i2c_w("0x32", "0x04", [v & 0xff, (v >> 8) & 0xff])

# ── V4L2 mmap capture (ctypes) ────────────────────────────────────────────
class _timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]
class _timecode(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("frames", ctypes.c_uint8), ("seconds", ctypes.c_uint8),
                ("minutes", ctypes.c_uint8), ("hours", ctypes.c_uint8),
                ("userbits", ctypes.c_uint8 * 4)]
class _buf_m(ctypes.Union):
    _fields_ = [("offset", ctypes.c_uint32), ("userptr", ctypes.c_ulong),
                ("fd", ctypes.c_int32)]
class v4l2_buffer(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("type", ctypes.c_uint32),
                ("bytesused", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("field", ctypes.c_uint32), ("timestamp", _timeval),
                ("timecode", _timecode), ("sequence", ctypes.c_uint32),
                ("memory", ctypes.c_uint32), ("m", _buf_m),
                ("length", ctypes.c_uint32), ("reserved2", ctypes.c_uint32),
                ("request_fd", ctypes.c_int32)]
class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("type", ctypes.c_uint32),
                ("memory", ctypes.c_uint32), ("capabilities", ctypes.c_uint32),
                ("flags", ctypes.c_uint8), ("reserved", ctypes.c_uint8 * 3)]

def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (t << 8) | nr
_V = ord('V')
VIDIOC_REQBUFS  = _IOC(3, _V, 8,  ctypes.sizeof(v4l2_requestbuffers))
VIDIOC_QUERYBUF = _IOC(3, _V, 9,  ctypes.sizeof(v4l2_buffer))
VIDIOC_QBUF     = _IOC(3, _V, 15, ctypes.sizeof(v4l2_buffer))
VIDIOC_DQBUF    = _IOC(3, _V, 17, ctypes.sizeof(v4l2_buffer))
VIDIOC_STREAMON = _IOC(1, _V, 18, ctypes.sizeof(ctypes.c_int))
VIDIOC_STREAMOFF = _IOC(1, _V, 19, ctypes.sizeof(ctypes.c_int))
BUF_TYPE = 1   # V4L2_BUF_TYPE_VIDEO_CAPTURE
MEM_MMAP = 1   # V4L2_MEMORY_MMAP

class Cap:
    def __init__(self, nbufs=8):
        self.fd = os.open(DEV, os.O_RDWR)
        req = v4l2_requestbuffers(count=nbufs, type=BUF_TYPE, memory=MEM_MMAP)
        fcntl.ioctl(self.fd, VIDIOC_REQBUFS, req)
        self.maps = []
        for i in range(req.count):
            b = v4l2_buffer(index=i, type=BUF_TYPE, memory=MEM_MMAP)
            fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, b)
            mm = mmap.mmap(self.fd, b.length, mmap.MAP_SHARED, mmap.PROT_READ,
                           offset=b.m.offset)
            self.maps.append(mm)
            fcntl.ioctl(self.fd, VIDIOC_QBUF, b)
        fcntl.ioctl(self.fd, VIDIOC_STREAMON, ctypes.c_int(BUF_TYPE))

    def grab(self):
        b = v4l2_buffer(type=BUF_TYPE, memory=MEM_MMAP)
        fcntl.ioctl(self.fd, VIDIOC_DQBUF, b)        # blocks for a complete frame
        data = self.maps[b.index][:FRAME_BYTES]      # copy out before requeue
        fcntl.ioctl(self.fd, VIDIOC_QBUF, b)         # return buffer to the ring
        return data


class Engine:
    def __init__(self):
        self.cap = Cap()
        self.shs1 = 400
        self.gain = 40
        _set_gain(self.gain); _set_shs1(self.shs1)
        self._lock = threading.Lock(); self._jpeg = b""; self._running = True
        self._fps = 0.0; self._tl = time.time(); self._mean = 0.0; self._frame_n = 0
        self._latest = None; self._llock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._loop, daemon=True).start()

    def _reader(self):
        while self._running:
            try:
                raw = self.cap.grab()
            except Exception:
                time.sleep(0.01); continue
            with self._llock:
                self._latest = raw

    def _ae(self, mean10):
        self._mean = mean10
        err = TARGET - mean10
        if abs(err) < 30:
            return
        if err > 0:
            if self.shs1 > SHS1_MIN:
                self.shs1 = max(SHS1_MIN, self.shs1 - max(4, int(self.shs1 * 0.25))); _set_shs1(self.shs1)
            elif self.gain < GAIN_MAX:
                self.gain = min(GAIN_MAX, self.gain + 24); _set_gain(self.gain)
        else:
            if self.gain > GAIN_MIN:
                self.gain = max(GAIN_MIN, self.gain - 24); _set_gain(self.gain)
            elif self.shs1 < SHS1_MAX:
                self.shs1 = min(SHS1_MAX, self.shs1 + max(4, int(self.shs1 * 0.25))); _set_shs1(self.shs1)

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
            den = cv2.medianBlur(f10.astype(np.uint16), 3).astype(np.float32)
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
                     f"AE: exp(SHS1)={self.shs1}  gain={self.gain}/480  (mmap, de-banded)"]
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
    if BUS is None:
        print("WARNING: camera i2c bus (addr 0x1a) not found — AE disabled", flush=True)
    ENGINE = Engine()
    print(f"\n  IMX296 live preview (V4L2 mmap, i2c AE bus {BUS}): http://0.0.0.0:{args.port}/\n", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
