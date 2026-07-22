#!/bin/sh
# smoke.sh - tier-3 loopback smoke: fake feeds -> fusiond -> endpoints.
# Uses non-production ports (18092/18095/18096). Exits nonzero on any failure.
set -e
cd "$(dirname "$0")/.."

fail() { echo "SMOKE: FAIL - $1"; kill $FEEDS_PID $FUS_PID 2>/dev/null || true; exit 1; }

./tools/fake_feeds 18092 18095 >/dev/null 2>&1 &
FEEDS_PID=$!
# run from tools/ so the trim-file fallback never touches a real rig state file
( cd tools && exec ../fusiond -p 18096 -r 127.0.0.1:18092 -t 127.0.0.1:18095 ) >/dev/null 2>&1 &
FUS_PID=$!

# wait for both feeds connected (up to 10 s)
i=0
while [ $i -lt 100 ]; do
    STATS=$(curl -s --max-time 2 "http://127.0.0.1:18096/stats" || true)
    case "$STATS" in
        *'"rad_connected":true'*'"trk_connected":true'*) break ;;
    esac
    i=$((i+1)); sleep 0.1
done
[ $i -lt 100 ] || fail "feeds never connected: $STATS"

# a fused row must appear on /stream within 5 s (marriage needs ~0.8 s of
# evidence now, so sample enough of the stream to cover several seconds)
STREAM=$(curl -s --max-time 5 "http://127.0.0.1:18096/stream" | head -c 150000 || true)
case "$STREAM" in
    *'"src":"fus"'*) : ;;
    *) fail "no fused row on /stream" ;;
esac
case "$STREAM" in
    *'"type":"fus"'*) : ;;
    *) fail "bad stream frame" ;;
esac

# /ctl round-trip: set a knob, see it echoed in /stats
curl -s --max-time 2 "http://127.0.0.1:18096/ctl?gate=2.0" | grep -q ok || fail "/ctl not ok"
sleep 0.3
curl -s --max-time 2 "http://127.0.0.1:18096/stats" | grep -q '"gate":2.00' || fail "knob not echoed"

# kill the feeds: heartbeat frames with both disconnected must keep flowing
kill $FEEDS_PID 2>/dev/null || true
sleep 4
HB=$(curl -s --max-time 5 "http://127.0.0.1:18096/stream" | head -c 4000 || true)
case "$HB" in
    *'"rad_connected":false'*) : ;;
    *) fail "no heartbeat after feed loss" ;;
esac

kill $FUS_PID 2>/dev/null || true
rm -f tools/fusion-trim.json
echo "SMOKE: PASS"
