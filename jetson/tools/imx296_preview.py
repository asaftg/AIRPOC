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
import argparse, collections, ctypes, fcntl, glob, json, mmap, os, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import cv2
import numpy as np

DEV = "/dev/video0"

def _probe_fmt(dev=DEV):
    """Probe the driver's real format so geometry tracks the sensor ROI
    (1440x1088, stride 2880 = 64-byte aligned -> no VI line pad, no comb).
    Hardcoding a stale width would misreshape and shear the image."""
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
PORT = 8091
DISP_W_WIDE = 900          # bandwidth-friendly display width (use over WiFi)
JPEG_QUALITY = 90
ENGINE_MAX_FPS = 60        # match the 60 fps sensor; actual display fps is measured
GAMMA = 0.85
TARGET = 450.0
ADDR = "0x1a"
SHS1_MIN, SHS1_MAX = 8, 1100
GAIN_MIN, GAIN_MAX = 0, 480
BLACK = 60.0

# ── sensor timing (1440x1088 @ 60 fps mode) — for exposure/duty math ──────────
VMAX = 1125                              # frame length in lines
LINE_US = 1100.0 / 74_250_000 * 1e6      # HMAX / pixel_clock = 14.815 us per line
FRAME_US = VMAX * LINE_US                # 16667 us (60 fps)
MAX_EXP_LINES = VMAX - SHS1_MIN          # 1117 = longest integration at 60 fps
MIN_EXP_LINES = VMAX - SHS1_MAX          # 25
# duty = exposure_time / frame_time = exposure_lines / VMAX (== NIR strobe duty)

# ── lens + sensor geometry for FOV: CommonLands CIL122 f=12mm, IMX296 3.45um ──
FOCAL_MM = 12.0
PIX_UM = 3.45
ZOOMS = (1, 2, 4, 8)

_GAMMA_LUT = np.array([((i / 255.0) ** (1.0 / GAMMA)) * 255.0
                       for i in range(256)], dtype=np.uint8)

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

def _apply_exposure(shs1, gain):
    """Atomically latch SHS1+gain via the IMX296 REGHOLD (CTRL08 0x3008): a
    mid-stream update otherwise lets a frame see partial/mismatched values, which
    is the band-shift tearing. REGHOLD=1, write both, REGHOLD=0 -> latch at VSYNC."""
    shs1 = int(max(SHS1_MIN, min(SHS1_MAX, shs1)))
    gain = int(max(GAIN_MIN, min(GAIN_MAX, gain)))
    _i2c_w("0x30", "0x08", [0x01])                                          # REGHOLD on
    _i2c_w("0x30", "0x8d", [shs1 & 0xff, (shs1 >> 8) & 0xff, (shs1 >> 16) & 0xff])
    _i2c_w("0x32", "0x04", [gain & 0xff, (gain >> 8) & 0xff])
    _i2c_w("0x30", "0x08", [0x00])                                          # REGHOLD off -> latch

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
        self.exp_lines = 725                 # integration in lines; shs1 = VMAX - exp_lines
        self.gain = 40                       # 0..480 (0.1 dB/step)
        self.shs1 = VMAX - self.exp_lines
        self.mean_ema = TARGET               # filtered metric -> smooth, flicker-free AE
        self.zoom = 1                        # digital zoom 1/2/4/8 (center crop)
        self.native = False                  # False -> 900px display, True -> full 1440
        _apply_exposure(self.shs1, self.gain)
        self._lock = threading.Lock(); self._jpeg = b""; self._jpeg_id = 0
        self._running = True
        self._fps = 0.0; self._tl = time.time(); self._mean = 0.0; self._frame_n = 0
        self._latest = None; self._llock = threading.Lock()
        self.trace = collections.deque(maxlen=300)   # (t, mean, shs1, gain, exp_us, duty)
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._loop, daemon=True).start()

    def _gain_lin(self, g):
        return 10.0 ** (g * 0.1 / 20.0)      # register step 0.1 dB -> linear brightness

    def _fov(self):
        w_mm = (W * PIX_UM / 1000.0) / self.zoom
        h_mm = (H * PIX_UM / 1000.0) / self.zoom
        return (float(np.degrees(2 * np.arctan(w_mm / (2 * FOCAL_MM)))),
                float(np.degrees(2 * np.arctan(h_mm / (2 * FOCAL_MM)))))

    def _reader(self):
        while self._running:
            try:
                raw = self.cap.grab()
            except Exception:
                time.sleep(0.01); continue
            with self._llock:
                self._latest = raw

    def _ae(self, mean10):
        # Flicker-free AE: filter the metric (EMA), act in the log/multiplicative
        # domain with damping + a per-update slew cap, and use a wide deadband.
        # The old loop took fixed 25%-exposure / 24-gain steps that overshot the
        # deadband near a bright light -> pumping. Here a single "brightness" B =
        # exposure_lines x gain_linear is nudged toward target and re-decomposed
        # (exposure first for low noise, gain only when exposure is maxed).
        self._mean = mean10
        self.mean_ema = 0.6 * self.mean_ema + 0.4 * mean10
        ratio = TARGET / max(1.0, self.mean_ema)
        if 0.90 < ratio < 1.11:              # +/-10% deadband -> no hunting
            return
        factor = ratio ** 0.5                # damping: correct half the error/update
        factor = min(1.5, max(0.667, factor))  # slew cap: <=1.5x brightness per update
        B = self.exp_lines * self._gain_lin(self.gain) * factor
        exp = min(MAX_EXP_LINES, max(MIN_EXP_LINES, B))   # spend exposure first
        g_lin_needed = max(1.0, B / exp)                  # remainder -> analog gain
        g = int(round(20.0 * np.log10(g_lin_needed) / 0.1))
        self.exp_lines = int(round(exp))
        self.gain = int(min(GAIN_MAX, max(GAIN_MIN, g)))
        self.shs1 = int(min(SHS1_MAX, max(SHS1_MIN, VMAX - self.exp_lines)))
        _apply_exposure(self.shs1, self.gain)

    def _loop(self):
        period = 1.0 / ENGINE_MAX_FPS
        while self._running:
            t0 = time.time()
            with self._llock:
                raw = self._latest; self._latest = None
            if raw is None:
                time.sleep(0.004); continue
            # Stride is the driver's real bytesperline (BPL); slice to W. The
            # sensor ROI crop (1440) makes the Y10 line 64-byte aligned, so the
            # Tegra VI adds no per-line pad and there is NO even/odd comb to
            # correct here -- the fix is at the source, not in software.
            f10 = (np.frombuffer(raw, dtype="<u2").reshape(H, BPL // 2)[:, :W] >> 6)
            now = time.time(); dt = now - self._tl; self._tl = now
            if dt > 0:
                inst = 1.0 / dt
                self._fps = 0.85 * self._fps + 0.15 * inst if self._fps else inst
            self._frame_n += 1
            if self._frame_n % 4 == 0:
                self._ae(float(f10[::8, ::8].mean()))
            # digital zoom = centered crop (upscaled to the display size below)
            z = self.zoom if self.zoom in ZOOMS else 1
            if z > 1:
                ch, cw = H // z, W // z
                y0, x0 = (H - ch) // 2, (W - cw) // 2
                view = f10[y0:y0 + ch, x0:x0 + cw]
            else:
                view = f10
            # Resize to the display size FIRST, then tone-map on the smaller image
            # -> the heavy per-pixel ISP runs on ~1/2.5 the pixels, lifting fps
            # toward 60. INTER_AREA (when downscaling) averages out hot pixels, so
            # no median is needed; the sensor line is already clean (no de-band).
            # No row de-band: raw row-FPN ~0.5 LSB; a content-derived one bleeds
            # horizontal edges into moving streaks (see IMAGE_PIPELINE.md).
            dispw = W if self.native else DISP_W_WIDE
            disph = int(dispw * H / W)
            interp = cv2.INTER_AREA if view.shape[1] >= dispw else cv2.INTER_LINEAR
            small = cv2.resize(view.astype(np.float32), (dispw, disph), interpolation=interp)
            white = float(np.percentile(small[::2, ::2], 99.5))
            if white <= BLACK + 1:
                white = BLACK + 1
            y8 = np.clip((small - BLACK) * (255.0 / (white - BLACK)), 0, 255).astype(np.uint8)
            y8 = cv2.LUT(y8, _GAMMA_LUT)
            disp = cv2.cvtColor(y8, cv2.COLOR_GRAY2BGR)
            exp_us = self.exp_lines * LINE_US
            duty = 100.0 * self.exp_lines / VMAX
            hf, vf = self._fov()
            lines = [f"IMX296 Y10  {self._fps:.0f} fps  mean={self._mean:.0f}/1023",
                     f"exp={exp_us / 1000:.2f}ms  duty={duty:.0f}%  gain={self.gain}/480",
                     f"FOV {hf:.1f}x{vf:.1f}deg  zoom {z}x  {W if self.native else DISP_W_WIDE}px"]
            yt = disph - 12
            for ln in reversed(lines):
                cv2.putText(disp, ln, (10, yt), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 3, cv2.LINE_AA)
                cv2.putText(disp, ln, (10, yt), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 1, cv2.LINE_AA)
                yt -= 26
            ok, b = cv2.imencode(".jpg", disp, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            if ok:
                with self._lock:
                    self._jpeg = b.tobytes(); self._jpeg_id = self._frame_n
            if self._frame_n % 2 == 0:
                self.trace.append((now, self._mean, self.shs1, self.gain, exp_us, duty))
            slack = period - (time.time() - t0)
            if slack > 0:
                time.sleep(slack)

    def get(self):
        with self._lock:
            return self._jpeg


ENGINE = None
HTML = ("""<!DOCTYPE html><html><head><title>IMX296 Live</title>
<style>
 body{background:#111;margin:0;font-family:monospace;color:#0f0}
 #wrap{display:flex;flex-direction:column;align-items:center}
 img{max-width:100vw;max-height:88vh}
 .bar{padding:6px;display:flex;gap:6px;flex-wrap:wrap;align-items:center}
 button{background:#222;color:#0f0;border:1px solid #0a0;padding:6px 10px;cursor:pointer}
 button.on{background:#0a0;color:#000}
 span{color:#6f6;margin:0 6px}
</style></head><body><div id="wrap">
 <div class="bar">
  <span>zoom</span>
  <button onclick="z(1)" id="z1">1x</button><button onclick="z(2)" id="z2">2x</button>
  <button onclick="z(4)" id="z4">4x</button><button onclick="z(8)" id="z8">8x</button>
  <span>res</span>
  <button onclick="r(0)" id="r0">900</button><button onclick="r(1)" id="r1">native 1440</button>
 </div>
 <img src="/stream">
</div><script>
 function z(v){fetch('/ctl?zoom='+v);[1,2,4,8].forEach(i=>document.getElementById('z'+i).className=(i==v)?'on':'')}
 function r(v){fetch('/ctl?native='+v);document.getElementById('r0').className=v?'':'on';document.getElementById('r1').className=v?'on':''}
 z(1);r(0);
</script></body></html>""").encode()


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
            last = -1
            try:
                while True:
                    with ENGINE._lock:
                        j = ENGINE._jpeg; jid = ENGINE._jpeg_id
                    if j and jid != last:                # push each new frame promptly
                        last = jid
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                         + str(len(j)).encode() + b"\r\n\r\n" + j + b"\r\n")
                    else:
                        time.sleep(0.002)
            except (BrokenPipeError, ConnectionResetError): pass
        elif self.path.startswith("/ctl"):
            q = self.path.split("?", 1)[1] if "?" in self.path else ""
            for kv in q.split("&"):
                if "=" not in kv:
                    continue
                k, v = kv.split("=", 1)
                if k == "zoom" and v.isdigit() and int(v) in ZOOMS:
                    ENGINE.zoom = int(v)
                elif k == "native":
                    ENGINE.native = (v == "1")
            self.send_response(200); self.send_header("Content-Length", "2")
            self.end_headers(); self.wfile.write(b"ok")
        elif self.path.startswith("/stats"):
            hf, vf = ENGINE._fov()
            body = json.dumps({
                "fps": round(ENGINE._fps, 1), "zoom": ENGINE.zoom,
                "native": ENGINE.native, "hfov_deg": round(hf, 2), "vfov_deg": round(vf, 2),
                "cols": ["t", "mean", "shs1", "gain", "exp_us", "duty_pct"],
                "trace": [[round(t, 3), round(m, 1), s, g, round(e, 1), round(d, 1)]
                          for (t, m, s, g, e, d) in list(ENGINE.trace)],
            }).encode()
            self.send_response(200); self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body))); self.end_headers()
            self.wfile.write(body)
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
