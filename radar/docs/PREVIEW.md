# Radar previewer

A standalone half-circle **PPI** (plan-position indicator), served on `:8092`,
built before the GUI so the radar can be seen and tuned on its own. Parity
target: the point cloud + target boxes the ground-bench GUI showed.

## Reading it

- **Origin** bottom-centre; **+y (boresight) is up**, `+x` right.
- **Range rings** at ¼ increments; the **100 m** ring is amber. The view
  "breathes" — parked at 100 m, jumps to 500 m when a confirmed target passes
  ~105 m, shrinks back after 30 quiet frames.
- **FOV wedge** = the ±30° azimuth gate of the A/G profile.
- **Points** coloured by Doppler: **red approaching**, **blue receding**, **dim
  cyan static** (|v| < 0.2 m/s). Opacity scales with SNR when present.
- **Target boxes**: **solid** = live (measured this frame), **dashed/dim** =
  coasting (dead-reckoned through a dropout). Arrow = 1 s of velocity; label is
  `R#<id>  <speed> m/s · <range> m`. IDs are transient radar-track ids
  (`R#`) — fusion assigns global ids later.
- **Trails** = per-track history, EMA-smoothed, aged out over 10 s.
- **HUD** (top-left) = profile, Hz, point/target counts, dropped-frame count.

## Transport

Frames arrive over **SSE** (`EventSource("/stream")`), one JSON object per
radar frame. SSE was chosen over WebSocket: a fraction of the code, and the
right fit for one-way structured push. Stats poll `/stats` at 4 Hz.

The daemon publishes a finished snapshot per frame; browsers only ever read the
latest snapshot, so a slow client drops **display** frames and can never stall
the drop-free UART→parse pipeline.
