# Offload — Jetson → workstation (bench pull)

v1 policy: recordings leave the Jetson only via **workstation-initiated pull**
over ssh (WiFi normally, GigE when cabled). Nothing on the Jetson compresses or
uploads during field ops. Two interchangeable tools:

### Windows (zero setup) — `tools/airpoc-offload.ps1`
Native PowerShell using the ssh/scp/tar that ship with Windows 10/11 — no WSL,
no rsync install. Tars the selected channels on the Jetson, scp's the tar
(binary-safe; PowerShell pipes are not), extracts locally.
```
# annotations + display video for every saved session -> D:\airpoc
.\airpoc-offload.ps1 -RemoteHost asaftg@orin-nano -Tier display -Dest D:\airpoc
# one session, full raw native, then free the raw on the Jetson once it's local:
.\airpoc-offload.ps1 -RemoteHost asaftg@orin-nano -Sid 20260705T070724Z -Tier full -PruneNativeAfter
```
Tiers: `meta` (manifest+thumbs+radar+events, MBs) · `display` (+ the operator's
video, ~0.2-1 GB/session) · `full` (+ raw native Y10, 7-37 GB/session).

### Resumable big pulls — `tools/offload_pull.sh` (WSL/Linux)
For large native pulls over flaky WiFi, run the bash tool under WSL: rsync
`--partial` (resumes after a dropped link) + zstd wire compression.

### The plan / roadmap
- **Now (v1):** the two pull tools above, run by hand when the Jetson is docked.
  Tiered so you can grab everything-but-raw in seconds and pull raw on demand.
- **Next (v2, GUI hook):** a per-session "Offload" affordance in the LIBRARY tab
  that marks a session and shows a state chip; the actual bytes still move via
  the pull tool (the operator laptop, not the Jetson, initiates). The recorder
  already carries `offload:{state}` in the manifest for this; the GUI work-order
  notes it as a future control. `purge_native` (drop raw, keep the rest) is the
  reclaim path once a session is safely offloaded.
- **Explicitly not in scope:** the Jetson pushing to cloud/Drive on its own
  (keeps the field device dumb and offline); large-native over the drone RF link.

The rsync details below are the WSL tool.

```
tools/offload_pull.sh asaftg@orin-nano [--meta|--display|--full] [--prune-native] [dest_dir]
```

- Tiers: `--meta` (manifest+thumbs+radar+events — MBs), `--display` (+eo_jpeg,
  ~0.2–1 GB per 1–5 min session), `--full` (+eo_y10 raw — 7–37 GB per session).
- Two passes by design: meta+display for ALL sessions first (browsable on the
  workstation within minutes), raw afterwards. Resumable (`rsync --partial`).
- Wire compression: `--compress-choice=zstd` — the Jetson compresses only
  during the transfer (idle time). ~1.3–1.5× on sensor noise.
- `--prune-native` runs `purge_native` on the Jetson per session **after** the
  local copy verifies (per-record CRC via `airec_dump.py --verify`).
- Archival: `tools/compress_native.sh <session>` on the WORKSTATION converts
  eo_y10 to FFV1 (gray16, lossless, ~2–3×) for storage.

Escape hatches if raw-over-WiFi becomes the bottleneck: plug the GigE cable
(~6×), or an on-Jetson idle zstd pre-pass (not built; would be an explicit
"compact" job, never concurrent with recording). Future options intentionally
out of scope in v1: rclone→cloud, GUI tar download.

> Pitfall: rsync needs ≥3.2 on BOTH ends for `--compress-choice=zstd`; the
> script falls back to `-z` (zlib) automatically when either side is older.
