# Radar previewer

A standalone half-circle **PPI** (plan-position indicator), served on `:8092`,
built before the GUI so the radar can be seen and tuned on its own. Parity
target: the point cloud + target boxes the ground-bench GUI showed.

## Reading it

- **Origin** bottom-centre; **+y (boresight) is up**, `+x` right.
- **Range rings** at ¼ increments; the **100 m** ring is amber. The view
  "breathes" — parked at 100 m, jumps to 500 m when a confirmed target passes
  ~105 m, shrinks back after 30 quiet frames.
- **FOV wedge** = the cfg's published azimuth span (±90°; useful AoA ~±60°).
- **Points** coloured by Doppler: **red approaching**, **blue receding**, **dim
  cyan static** (|v| < 0.2 m/s). Opacity scales with SNR when present.
- **Target boxes**: one per cluster **detected this frame** (no coasting — a box
  that stops clustering just disappears). Arrow = 1 s of velocity; label is
  `R#<id>  <speed> m/s · <range> m`. IDs are transient radar-track ids (`R#`,
  stable frame-to-frame via association) — fusion assigns global ids later.
- **HUD** (top-left) = profile, Hz, point/target counts, dropped-frame count.

The previewer draws only the current frame — no trails or persistence. Holding
a box through a one-frame miss (fade) is the operator GUI's job, not the radar's.

## Transport

Frames arrive over **SSE** (`EventSource("/stream")`), one JSON object per
radar frame. SSE was chosen over WebSocket: a fraction of the code, and the
right fit for one-way structured push. Stats poll `/stats` at 4 Hz.

The daemon publishes a finished snapshot per frame; browsers only ever read the
latest snapshot, so a slow client drops **display** frames and can never stall
the drop-free UART→parse pipeline.
