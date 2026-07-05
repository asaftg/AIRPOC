# Offload — Jetson → workstation (bench pull)

v1 policy: recordings leave the Jetson only via **workstation-initiated rsync
pull** over ssh (WiFi normally, GigE when cabled). Nothing on the Jetson
compresses or uploads during field ops.

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
