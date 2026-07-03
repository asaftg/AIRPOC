# Radar module — status & future work

## Status (2026-07-02)

Working and **verified on the board**: aarch64 build, cfg push, 3.125 Mbaud
UART, **20 Hz / 0 dropped frames**, per-point **SNR live** (~16–50 dB),
range-adaptive DBSCAN → class-less per-frame boxes, SSE previewer with a live
tuning panel (ε / min-pts / min-speed / min-SNR via `/ctl`). Detections are
class-less; no coasting (that's the tracking module's job). See
[`README.md`](../README.md).

## Future work — ordered by value / readiness

### 1. Tune the detector against real returns (near-term, high value, cheap)
Nothing here needs new code — it needs the board and moving targets. Walk
people and drive vehicles at known ranges and set the parameters empirically
(procedure + which knobs in [`TUNING.md`](TUNING.md)). Priorities: the
`min SNR` default (now that SNR is live), the `ε range slope` (0.06, unverified),
and the doppler gate (3). This is the single best next step.

### 2. Record + replay (enables repeatable tuning/regression)
Add a way to record the raw TLV stream to a file and replay it through the
daemon (a `-r <file>` mode next to `-s` sim). Lets us tune and regression-test
offline against captured scenes instead of needing a live walk every time. The
ground bench had recordings; we should too.

### 3. Expose more knobs live (small)
Promote `EPS_RANGE_SLOPE` and `EPS_DOP_MPS` from `#define` to `/ctl` params so
we can tune them from the previewer without rebuilds (like ε/min-pts today).

### 4. Sensitivity: use SNR to trade false-alarms vs range (medium)
Now that per-point SNR is live, explore: lower the CFAR floor (toward ~16 dB)
to detect farther, and lean on a host `min SNR` gate + SNR-weighted cluster
centroids to keep false alarms down. Requires a power-cycle to re-push a lower
CFAR cfg; measure human detection range before/after.

### 5. Frame rate → ~30 Hz (small, HW-gated)
Tighten `frameCfg` period toward the ~30 ms active dwell (N=384 / 128 loops
unchanged, so no sensitivity loss). Needs a power-cycle re-push and a DSP-
headroom check (0 dropped frames). See [`FIRMWARE.md`](FIRMWARE.md).

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
