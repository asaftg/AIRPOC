"""Light IMX296 Y10 -> GRAY8 capture to stdout, for a streaming pipeline.

Minimal per-frame work (read + >>8) so it sustains the sensor's 60 fps and the
downstream encoder never starves. No ISP here (the de-band/AE quality path is
imx296_preview.py); this is the max-fps feed for jetson/streaming.

  python3 imx296_capture8.py | gst-launch-1.0 fdsrc ! rawvideoparse ...
"""
import subprocess, sys
import numpy as np

W, H = 1456, 1088
FRAME_BYTES = W * H * 2
DEV = "/dev/video0"


def main():
    proc = subprocess.Popen(
        ["v4l2-ctl", "-d", DEV, "--stream-mmap", "--stream-count=0", "--stream-to=-"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
    out = sys.stdout.buffer
    while True:
        buf = bytearray()
        while len(buf) < FRAME_BYTES:
            c = proc.stdout.read(FRAME_BYTES - len(buf))
            if not c:
                return
            buf += c
        g = (np.frombuffer(bytes(buf), dtype="<u2") >> 8).astype(np.uint8)  # left-just Y10 high byte
        out.write(g.tobytes())


if __name__ == "__main__":
    main()
