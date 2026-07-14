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
# MAXN_SUPER is ID 2 on the Orin Nano Super (VERIFIED on-box in /etc/nvpmodel.conf).
# ID 0 is the 15W restricted mode, ID 1 is 25W, ID 3 is 7W — do NOT use -m 0 (it caps
# the GPU at 612 MHz). The old docs/agent note that said "-m 0 = MAXN_SUPER" was wrong.
nvpmodel -m 2 </dev/null      # MAXN_SUPER (no reboot needed for MAXN modes)
jetson_clocks                 # lock CPU/GPU/EMC to max, disable DVFS
# Belt-and-suspenders: force the CPU governor to performance (explicit + idempotent).
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g" 2>/dev/null || true
done
