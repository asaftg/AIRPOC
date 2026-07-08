# datasets/tools — pipeline stages + helpers

Each `ds_*.py` is one stage with a single responsibility; they hand off through
files under `DATASET_ROOT` (see `../SPEC.md`), never in-memory, so any stage can
re-run or resume independently. All take `--root` (or the `DATASET_ROOT` env
var). Pure logic lives in the `airpoc_ds/` package and is unit-tested.

## Stages

| tool | job | network | GPU |
|---|---|---|---|
| `ds_catalog_fetch.py` | clone + parse catalog (geo CSV + annotation segments), pin commit, snapshot | GitHub | – |
| `ds_download.py` | mirror MP4s from CloudFront (resumable, content-addressed) | CDN | – |
| `ds_probe.py` | ffprobe → geometry/fps/pix_fmt/color_range/sha256 | – | – |
| `ds_inventory.py` | inventory + rights memo (deliverable) | – | – |
| `ds_extract.py` | decode → mono frames; exact terminal windows; dedup; exclusion masks | – | – (ffmpeg) |
| `ds_label.py` | GroundingDINO, tiled + multi-scale; `--rescan` for empty dive frames | HF weights | **yes** |
| `ds_label_crosscheck.py` | RTMDet COCO cross-check, same tiling | model zoo | **yes** |
| `ds_merge.py` | route via synonyms, auto-accept agreements, build capped review queue | – | – |
| `ds_split.py` | deterministic class-aware split **by video** | – | – |
| `ds_pack.py` | assemble COCO, fold in review, re-assert every invariant | – | – |
| `ds_stats.py` | class counts + box-size histograms (source px + seeker-referenced) | – | – |
| `ds_manifest.py` | hashes + counts → dataset card, freeze version | – | – |
| `ds_validate.py` | **the gate**: schema + invariants + hash recompute | – | – |
| `ds_make_fixture.py` | synthesize an offline fixture to test the non-GPU spine | – | – |

## `airpoc_ds/` package

`__init__` (constants, `DATASET_ROOT` resolution, atomic JSON I/O) ·
`hashing` (sha256 + deterministic ids) · `luma` (frozen BT.601/709 →8-bit) ·
`dedup` (dHash + temporal window) · `terminal` (segment windows + looming
fallback + the sample plan) · `catalog` (geo CSV + annotation join) ·
`tiling` (SAHI-style tiled multi-scale + NMS) · `coco` (record builders, IoU,
clip) · `review` (toughness + budget cap) · `ledger` (exclusions) ·
`schema` (load + validate against `../schema/`).

## Install

```
pip install -r requirements.txt          # core: numpy Pillow jsonschema pytest
```
GPU labelers (`ds_label*`) additionally need torch + transformers + mmdet, and
`ds_extract`/`ds_probe` need ffmpeg on PATH — see the notes in
`requirements.txt`. The core spine + tests need none of that.

## Test

```
pytest tools/tests            # helpers (luma/dedup/terminal/split/schema) + full fixture spine
```
