#!/bin/bash
# IMX296 ~58 fps low-latency MJPEG stream over RTP/UDP.
# (Orin Nano has no HW video encoder; software H.264 only manages ~6 fps, so the
#  high-fps path is MJPEG. For HW H.264 use the Xavier AGX target.)
# View:  gst-launch-1.0 udpsrc port=PORT caps="application/x-rtp,encoding-name=JPEG,payload=26" ! rtpjpegdepay ! jpegdec ! autovideosink
HOST=${1:-127.0.0.1}; PORT=${2:-5000}; EXP=${3:-16000}; GAIN=${4:-120}
pkill -f imx296_capture8 2>/dev/null; pkill -9 v4l2-ctl 2>/dev/null; sleep 1
v4l2-ctl -d /dev/video0 --set-ctrl exposure=$EXP,gain=$GAIN 2>/dev/null
echo "streaming MJPEG/RTP to $HOST:$PORT (exp=$EXP gain=$GAIN)"
python3 "$HOME/imx296_capture8.py" 2>/dev/null | gst-launch-1.0 -q \
  fdsrc ! rawvideoparse format=gray8 width=1456 height=1088 framerate=60/1 ! \
  jpegenc quality=72 ! rtpjpegpay ! udpsink host=$HOST port=$PORT sync=false
