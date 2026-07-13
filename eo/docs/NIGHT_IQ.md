# Night image quality — what limits it and what `tdn.c` does

The night image is **read-noise-limited underexposure**: with exposure railed at the
operating fps and analog gain at its cap, the diffuse scene sits a few LSB above black.
The display tonemap stretches that ~6×, which turns ~0.7 LSB of *temporal* row/pixel
read-noise into visible grain + horizontal banding. Resolution is not the limiter —
the 12 mm f/2.0 lens resolves to the sensor's Nyquist (~1.4 px edges, measured).

**What actually helps, in order:** photons (illuminator reach/wavelength — see the
system plan), then temporal integration of static content. What does NOT help: any
single-frame filter (the noise is temporal; a spatial/row filter fights scene
structure — tried, shipped, ghosted daytime scenes, reverted), multi-frame
super-resolution (Nyquist-sampled sensor, nothing aliased to recover), pixel binning
(trades away pixels-on-target = DRI range).

## `tdn.c` — motion-adaptive temporal IIR (display-only)

Raw-domain (pre-tonemap, the noise is below the 8-bit floor), Q10.5 accumulator,
~3.3× static-scene noise cut, banding averages out. Key design points and why:

| Choice | Why (the failure it prevents) |
|---|---|
| 4×4 **block-pooled** motion test | a per-pixel `|Δ|>kσ` test is provably blind to a far slow walker (0.27 px/frame ⇒ ~2 LSB/frame < any usable threshold); pooling 16 px integrates the mover's contrast so it trips the test and stays crisp |
| AE step ⇒ **scale** accumulator by applied exposure×gain ratio | a reset throws away the integrated SNR (visible noise pulse); scaling is exact in the linear domain. Uses the +2-frame register-landing model (`eo_frame_ae`) — never "we wrote the register this frame" |
| **Empirical** per-intensity-bin noise scale | read-noise-only σ underestimates on lit pixels (shot noise dominates at high gain) ⇒ false motion exactly on the illuminated target |
| Error-feedback accumulation | Q10.5 truncation dead-band = stuck sub-LSB bias × 6 stretch = fixed pattern |
| Row offset vs the **accumulator reference**, static pixels only, ±3 LSB clamp; masked-out rows get **zero** correction | scene-referenced destripe eats horizontal structure (the reverted daytime-ghosting bug); neighbor-row interpolation injects uncorrelated noise |
| Global-motion guard (>60 % blocks moving ⇒ pass-through + reseed) | slew/unmodeled events; static-mount instrument, **interim** until a warp-accumulation design for the tracking gimbal |
| Night gate on **applied gain**, hysteresis | day = zero cost, byte-identical display path |

**Display-only by red-team verdict:** temporal denoise attenuates the faint slow
movers detection exists for (a 6 px slow drone keeps ~30 % contrast). The detector
keeps the **raw** tap and its own median-background mover head; the NN trains on
raw + noise augmentation. Do not feed `tdn` output to detection.

Knob: `/ctl?denoise=0|1`, telemetry `denoise / dn_active / dn_ms` in `/stats`
(`dn_ms` is the measured per-frame cost — the operator can reclaim the CPU live).
Validation: `make tdn_bench && ./tdn_bench /data/recordings/<sid>/eo_y10 /tmp` replays
recordings through the exact shipped code (metrics + side-by-side PGMs).

> Pitfall: the tonemap is now v2 (Q10.5 interp + ordered dither). The recorder's
> vendored replay tonemap must be updated to match or its drift guard will flag new
> recordings (by design).
