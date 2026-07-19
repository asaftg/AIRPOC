# Dataset SPEC — `layout-1` (FROZEN)

**This file is the recipe.** The packaging tools (`tools/ds_*.py`) and any training
pipeline both build against it. The machine-checkable half lives in
[`schema/`](schema/) (JSON Schemas + frozen category/synonym tables) — this file
explains the layout and the rules the schemas can't express. **Changing anything
here means a new `spec_version`** (`layout-2`, …); tools stamp the version they
wrote into every artifact and `ds_validate.py` refuses mixed versions.

Classes are frozen: `{1: vehicle, 2: human}`. No drone class.

## 1. Where things live

Code, spec, and schemas are in the repo (`datasets/`). **The data never is.**
Every tool takes the dataset location from `--root` or the `DATASET_ROOT` env
var — never a hard-coded path. On the build machine this is the external
4 TB SanDisk SSD.

```
DATASET_ROOT/
  manifest.json                      dataset card + content hashes (§8) — written LAST
  catalog/
    catalog_snapshot.json            pinned catalog commit + per-video url/geo/segments
  videos/                            quarantined source mirror (sensitive; prunable)
    <id>.mp4
    <id>.probe.json                  ffprobe result + sha256 (§3)
  frames/
    <id>/<id>_<ms8>_<b|t>.png        8-bit single-channel PNG (§4)
  raw_labels/
    <id>.gdino.json                  GroundingDINO raw output (§5)
    <id>.rtmdet.json                 RTMDet cross-check raw output (§5)
  annotations/
    instances_train.json             COCO, schema/coco_instances.schema.json
    instances_val.json
    instances_test.json
  splits/split_manifest.json         schema/split_manifest.schema.json
  review/review_queue.json           schema/review_queue.schema.json
  review/corrections.json            written by the review app
  exclusions/exclusions.json         honesty ledger, schema/exclusions.schema.json
  reports/inventory.{md,json}        catalog + rights memo (deliverable)
  reports/stats.{md,json}[,.png]     class/size stats (§7)
  logs/                              per-tool run logs
```

## 2. Identity

- **`video_id`** = `<slug>_<sha10>`: `slug` = the catalog stem
  (`YYYY-MM-DD_description`) truncated to 40 chars, `[a-z0-9_-]` only;
  `sha10` = first 10 hex of the **sha256 of the MP4 bytes**. Content-addressed:
  the catalog renames files over time, the hash doesn't move. A second URL whose
  bytes hash identically is recorded once; a re-encode that hashes differently is
  flagged `DUPLICATE_VIDEO` in exclusions for a human call — never silently merged.
- **Frame file** = `<video_id>_<ms8>_<mode>.png`; `ms8` = zero-padded
  timestamp in ms from stream start; `mode` = `b` (baseline) or `t` (terminal).
- **COCO `image.id`** = first 12 hex of `sha256("<video_id>:<ms8>:<mode>")` as an
  int (48 bits, JSON-safe). **COCO `annotation.id`** = first 12 hex of
  `sha256("<image_id>:<x>:<y>:<w>:<h>:<category_id>:<label_source>")` as an int.
  Both deterministic; `ds_pack` fails loudly on collision.

## 3. Mirror + probe

`ds_download.py` streams each MP4 from the CloudFront URL in the catalog
snapshot (resumable; sha256 recorded on completion). `ds_probe.py` writes
`<id>.probe.json`: `{duration_s, fps, width, height, pix_fmt, color_range,
codec, sha256, url, stem}`. Unreadable/corrupt videos → `PROBE_FAIL` /
`UNREADABLE_DECODE` in exclusions.

## 4. Frame extraction (`ds_extract.py`)

Frozen parameters (defaults; every deviation is recorded in the manifest):

| param | value |
|---|---|
| `baseline_fps` | 2.0 |
| `terminal_tail_s` | 6.0 |
| `terminal_fps` | 15.0 |
| dedup | dHash 64-bit, Hamming ≤ **4** within a 2 s window (**baseline only**); **terminal = disabled** (dHash is scale-tolerant and cannot safely tell a growing target from a repeat, so the closing sweep is kept whole and never decimated) |

- **Segment use** (from the catalog's human annotations, exact — the primary path):
  skip `banner_start` and `pause_start` regions; a *flight segment* runs from a
  `flight_start` to the next boundary of any type (or end-of-video). The **last
  `terminal_tail_s` of every flight segment** is sampled at `terminal_fps`
  (mode `t`); all remaining flight/replay footage at `baseline_fps` (mode `b`).
  Frames from `replay_start` regions get `from_replay: true` (near-duplicates of
  the live dive — training may down-weight them).
- **No annotations for a video** → interim fallback, stamped
  `terminal_source: "looming_heuristic"`: the tail window anchors on end-of-stream
  and is gated by a translation-invariant looming cue (foreground-area growth,
  smoothed; global FPV shake must NOT fire it — unit-tested).
- **`exclusion_masks`** from the annotation JSON are blanked to 0 in the pixels
  before anything sees the frame, and echoed into the image record.
- **Luma**: decode to RGB, convert with our own frozen coefficients — **BT.709**
  (0.2126/0.7152/0.0722) for height ≥ 720, **BT.601** (0.299/0.587/0.114) below;
  TV-range input (16–235) is expanded to full (0–255) first. The conversion used
  is recorded per image (`luma_conversion`, `orig_color_range`). Luma is a proxy
  for the seeker's mono sensor, not a spectral match — stated in the README.
- Ordinary (baseline) frames that end with **0 accepted boxes are dropped**
  (`EMPTY_GARBAGE`) — that's sky/blur/ground-rush. **Terminal frames** with 0
  detections trigger one harder re-scan (§5); still empty → dropped the same way,
  with at most a capped few routed to review as `terminal_no_labels`.

## 5. Labeling (`ds_label.py`, `ds_label_crosscheck.py`, `ds_merge.py`)

- **GroundingDINO** (primary, open-vocab) runs **tiled + multi-scale**
  (SAHI-style: 800 px tiles, 25 % overlap, scales 1.0/1.5/2.0, class-wise NMS
  IoU 0.5 on the merged set). Prompt and label→class mapping are frozen in
  [`schema/class_synonyms.json`](schema/class_synonyms.json) — mapping drift
  silently changes class counts, so it versions with this spec.
- **RTMDet** (COCO vocabulary) runs the same tiling as an independent
  cross-check; its `person/car/truck/bus/motorcycle` etc. map through the same
  synonym table.
- **Terminal re-scan**: a terminal frame with zero GroundingDINO boxes is re-run
  with 2× extra-zoomed tiles (400 px tiles, scale 2.0/3.0) before being declared
  empty.
- **Merge / auto-accept** (`ds_merge.py`), per raw GroundingDINO box:
  1. Map the raw label through the synonym table → `vehicle` / `human` /
     `REVIEW` / `IGNORE` (dropped).
  2. **Auto-accept** (`review_status: auto_ok`, `label_source: gdino+rtmdet`)
     iff score ≥ **0.45** AND an RTMDet box of the same mapped class overlaps
     with IoU ≥ **0.5**. The cross-check result is stored on the annotation.
  3. Otherwise it is a **review candidate** with `reasons` ⊆ {`low_conf`,
     `models_disagree`, `small_box_low_conf`, `crowded`, `terminal_no_labels`}.
- **Review queue is capped** (default budget **200 frames**, `--budget`), ranked
  by value: 10–40 px boxes first, terminal mode over baseline, larger model
  disagreement first. Overflow is **dropped and ledgered**
  (`REVIEW_OVERFLOW_DROP`) — never silently, never handed to the human.
  > The budget counts **queue items, and one item is one frame** carrying a list
  > of boxes (`ds_merge.py` appends per image; `review.py cap_queue` slices that
  > list). The number of boxes a reviewer actually sees is therefore **unbounded**
  > — a crowded frame can carry 8+ (`CROWDED_BOXES` is a tag threshold, not a
  > limit). Budget 200 does not mean 200 boxes.
- Human decisions from the review app land in `review/corrections.json`;
  `ds_pack` folds them back: `reviewed_ok` / `reviewed_edited` (with the fixed
  box/label, `label_source: human`, `score: 1.0`) / `rejected` (kept only in the
  ledger, never written to `instances_*.json`).

## 6. Split (`ds_split.py`)

By **`source_video_id`** — never by frame (frames of one video are
near-duplicates; a frame-level split leaks). Method `hash_stratified_v1`:
deterministic `sha1(video_id)` bucketing into ratios **80/10/10**
(train/val/test), stratified so videos containing `human` boxes spread across
all three splits (the rarer class must not starve val/test). Seed and full
assignment map are written to `splits/split_manifest.json`.

## 7. Packing, stats, validation

- `ds_pack.py` writes the three `instances_*.json` and **re-asserts**: every
  image's video is in exactly one split, every `image_id` unique and referenced,
  every bbox inside image bounds, every `category_id` ∈ {1, 2}, review budget
  respected, `indexed − used == exclusions`. Any failure aborts the pack.
- `ds_stats.py` reports class counts and the **box-size histogram in px with the
  10–40 px band called out**, each size given both in source pixels and rescaled
  to the seeker's 1440×1088 ("seeker-referenced" — this footage is assorted
  720p/1080p, not our geometry; the rescale is labelled, never silently applied).
- `ds_validate.py` is the reusable gate (the training pipeline runs it too):
  JSON-Schema-validates every artifact, re-checks the invariants above, and
  recomputes the manifest's content hashes.

## 8. Manifest (the seal)

`manifest.json` (schema: `schema/manifest.schema.json`) is written last by
`ds_manifest.py` and freezes: `spec_version`, catalog repo + pinned commit +
snapshot sha256, videos indexed/mirrored/used, tool + model versions, extraction
parameters actually used, per-class / per-mode / per-band counts, review
outcomes (auto-accepted / human-reviewed / dropped), split sizes, sha256 of each
`instances_*.json` + `split_manifest.json` + `stats.json`, `exclusions_count`,
and `use_restriction` (verbatim: catalog text is CC0; **the videos are
unlicensed third-party sensitive media — research/POC only, quarantined, never
redistributed**).

## 9. Exclusions ledger

Every indexed video or extracted frame that does **not** end up in the dataset
appears in `exclusions/exclusions.json` with a frozen `reason_code`:
`DOWNLOAD_FAIL · HASH_MISMATCH · DUPLICATE_VIDEO · PROBE_FAIL ·
UNREADABLE_DECODE · NO_LABELS · LABELS_LOW_CONF_ALL · EMPTY_GARBAGE ·
LICENSE_HOLD · MANUAL_EXCLUDE · REVIEW_OVERFLOW_DROP`.
`ds_validate.py` asserts the arithmetic closes. Honesty is a feature: counts in
reports are `used / indexed`, never just `used`.
