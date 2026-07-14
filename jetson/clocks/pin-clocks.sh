#!/bin/bash
# Pin the Jetson to deterministic max performance. jetson_clocks locks CPU/GPU/EMC to
# their max and disables DVFS, but it does NOT persist across reboots — so the box
# comes up on the schedutil governor and the GPU idles at ~half clock (510 of 1020 MHz).
# The detector's bursty ~15 fps inference never sustains load long enough for DVFS to
# boost, so every inference runs at half clock (~40 ms vs ~22 ms pinned) with a jittery
# p95. jetson-clocks.service runs this at boot so the pin survives reboots.
#
# Coexists with jetson-fan-max.service — pinning raises idle power/heat; the fan is
# already held at 100% (see fan/), so thermal is covered on the gimbal head.
set -e
nvpmodel -m 0 </dev/null      # MAXN_SUPER (no reboot needed for MAXN modes)
jetson_clocks                 # lock CPU/GPU/EMC to max, disable DVFS
# Belt-and-suspenders: force the CPU governor to performance (explicit + idempotent).
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null || true
done
