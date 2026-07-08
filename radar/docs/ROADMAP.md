# Radar module — status & future work

## Status (2026-07-08)

Working and **verified on the board**: aarch64 build, cfg push, 3.125 Mbaud
UART, **26 Hz / 0 dropped frames** (zero-compromise dead-time trim — see
[`FRAMERATE.md`](FRAMERATE.md)), per-point **SNR live** (~16–50 dB), the chip's
own DSP timing surfaced in `/stats` (`dsp_proc_ms`/`dsp_margin_ms`), a
**temporal multi-target tracker** → class-less target boxes (velocity from
position history, M-of-N confirm, short coast, park-hold, spatial dedup), SSE
previewer with a live tuning panel (**9** live `/ctl` knobs: dedup / min-pts /
min-speed / min-SNR / FOV / merge-gate / confirm / coast / park). Consumed
end-to-end by the GUI (`app/radar_client.c` → `:8092`). Detections are
class-less; the tracker confirms/coasts/park-holds, so the GUI no longer adds
its own persistence. Offline-validated against the session recording (detect
0.70, matches the Python reference). Daemon runs at ~1% CPU / <1 MB RSS. See
[`README.md`](../README.md) and [`TUNING.md`](TUNING.md).

## Future work — ordered by value / readiness

### 1. Re-tune across more recordings (near-term, high value)
Every tracker parameter was tuned against **one** recording (see the caveat in
[`TUNING.md`](TUNING.md)). As we capture more scenes — open field, heavy
clutter, faster movers, different mount height — re-validate and adjust. Tune the
live knobs first; touch the fixed numbers only when a whole behaviour is wrong;
and re-check **all** recordings before shipping a change so a fix for one scene
doesn't regress another. Single best next step; pairs with #2.

### 2. Offline scorer / regression harness (enables repeatable tuning)
Recording + replay already exist (the recorder taps raw radar + the wire; replay
serves recorded frames). What's missing is the **scorer**: a committed `tools/`
utility that replays a recording through the tracker and diffs the result
against ground truth (detect / false / latency / one-box-per-target). That is
what makes #1 repeatable — the tracker was validated exactly this way with bench
scaffolding; productizing it lets every future change (and the gtrack migration)
be regression-checked before it lands.

### 3. (done) Live control surface
The tracker exposes **9** live `/ctl` knobs (dedup, min-pts, min-speed, min-SNR,
FOV, merge-gate, confirm, coast, park). The remaining fixed internals (confirm
window, jitter gates, occupancy rates, association gates) are documented in
[`TUNING.md`](TUNING.md) and rarely need live tuning; promote one only if a real
scene shows it's needed.

### 4. Sensitivity: use SNR to trade false-alarms vs range (medium)
Now that per-point SNR is live, explore: lower the CFAR floor (toward ~16 dB)
to detect farther, and lean on a host `min SNR` gate + SNR-weighted cluster
centroids to keep false alarms down. Requires a power-cycle to re-push a lower
CFAR cfg; measure human detection range before/after.

### 5. Frame rate — 26 Hz SHIPPED; 30 Hz+ is firmware, and parked
**Done:** 26 Hz via a zero-compromise dead-time trim (idle/ramp), 0 drops. The
profile is DSP-bound at ~17 ms/frame (measured), so 25/30 Hz at the old timing
gave *zero* frames — the earlier "just tighten the period" idea was wrong. 30 Hz
clean needs a small firmware DSP trim; 40 Hz is a firmware stretch; 50–60 Hz is
impossible at full dwell. **Firmware frame-rate work is parked** — reflash only
earns its keep for *better detection*, never for Hz. Full analysis + the Phase-0
(measure the 17 ms split) recipe in [`FRAMERATE.md`](FRAMERATE.md).

### 6. On-chip Group Tracker (gtrack) — the one real remaining firmware item
Link TI's gtrack into our custom firmware so the chip emits target boxes
(TLV 308/309, already handled by `tlv.c`) and the host clusterer retires.
Offloads the Jetson and is TI's productized people/vehicle tracker. **Optional**
— host clustering works today, and full tracking is the downstream *tracking*
module's job regardless. Firmware build/flash process is documented in the
ground-bench repo.

### 7. AI / learned detection (big, data-gated — NOT killed)
A learned point-cloud detector (PointNet++-style segmentation → clustering, the
current research SOTA) could **materially improve human & vehicle range and
accuracy** — it can recognise a faint 2–3-dot smudge as a person, separate two
people walking together, and reject clutter hard enough to lower thresholds. It
can't invent signal from nothing, but it squeezes more out of what's there.

**Blocker: data.** We surveyed ~15 radar datasets — none are both commercially
usable *and* relevant (the good point-cloud sets are non-commercial; the
commercial ones are wrong-sensor RF tensors; none have drones). Radar detectors
are strongly sensor-specific, so even a licensed automotive set wouldn't
transfer to our seeker geometry. **Path = collect and label our own recordings
on this exact sensor** (which is also what item 2 sets up). Real effort, real
upside; a deliberate future investment, not a shortcut. (Drones have *no* usable
public data at all — but the human/vehicle upside alone can justify it.)

### 8. Fusion hand-off (other module)
Radar emits class-less detections with per-point SNR + velocity; classification
(person/vehicle) and cross-sensor association (radar + EO + thermal) belong to
the **fusion** module. Contract: [`INTEGRATION.md`](INTEGRATION.md).

## Known constraints (don't re-discover these)
- **One cfg per power-cycle** — the daemon reads-first and won't re-push to a
  live chip; a stuck sensor needs a radar power-cycle (see FIRMWARE.md).
- **No DCA** — raw-ADC / PMM / micro-Doppler paths are physically impossible on
  this build; radar is the UART point cloud only.
- **Per-point SNR** is a `guiMonitor detectedObjects` flag (1 = with), not a
  firmware feature to build.
