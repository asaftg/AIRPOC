# datasets — training-set builder (offline bench module)

Turns the **FPV drone-strikes Lebanon** video catalog into a train-ready
object-detection dataset for the AIRPOC EO detector: **grayscale** frames +
**COCO** labels, two classes — **vehicle** and **human** — with each strike's
final dive densely sampled (the small, closing-target case the detector most
needs). Auto-labeled by two independent models, confident agreements
auto-accepted, only a capped pile of genuinely hard boxes sent to a human.

This is **offline tooling** (Python, per the engineering guidelines — nothing
here runs on the seeker). It produces files; it is not part of the on-device
pipeline.

## I/O contract

**Consumes**
- The catalog repo `github.com/itamarwe/fpv-drone-strikes-lebanon-dataset`
  (metadata only; a pinned commit) and the source MP4s it references on
  CloudFront. The catalog is a moving target — every build pins its commit.
- Two open detectors for auto-labeling: GroundingDINO (Apache-2.0) and RTMDet
  (MMDetection, Apache-2.0). No Ultralytics/AGPL.

**Produces** — everything under `DATASET_ROOT` (an external drive, **never the
repo**; set `--root` or the `DATASET_ROOT` env var):
- `annotations/instances_{train,val,test}.json` — COCO, the training input.
- `manifest.json` — dataset card + content hashes + the use restriction.
- `reports/inventory.md` + `reports/stats.md` — what we have, box-size story.
- `exclusions/exclusions.json` — every video/frame that did **not** make it in,
  with a reason. Counts are always used/indexed, never just used.

The **stable contract** is `SPEC.md` (frozen `layout-1`) + `schema/*` — the
training pipeline builds against those, not against tool internals.
`ds_validate.py` is the gate both sides run.

## Where the data lives

On the external SanDisk SSD, referenced only via `DATASET_ROOT`. On this build
box (Windows + WSL) that is a drive like `E:\airpoc_ds` (`/mnt/e/airpoc_ds`
from WSL); on a Linux box it mounts under `/media/<user>/...`. Nothing large is
ever committed — `.gitignore` catches stray data.

## Build order

```
# metadata + mirror (network)
ds_catalog_fetch.py --root $R          # pin catalog commit, snapshot URLs+segments
ds_download.py      --root $R          # mirror MP4s from CloudFront (resumable)
ds_probe.py         --root $R          # ffprobe -> geometry/fps/color range
ds_inventory.py     --root $R          # the inventory + rights memo (deliverable)

# frames + labels (GPU for the two labelers)
ds_extract.py         --root $R        # sample mono frames (exact terminal windows)
ds_label.py           --root $R        # GroundingDINO, tiled + multi-scale
ds_label_crosscheck.py --root $R       # RTMDet cross-check
ds_merge.py           --root $R        # route/auto-accept; build capped review queue
ds_label.py --rescan  --root $R        # harder pass on empty terminal frames
ds_merge.py           --root $R        # (re-run after the rescan)

# human pass (optional but recommended)
python ../review_app/ds_review_server.py --root $R   # accept/fix/reject in a browser

# finalize + gate
ds_split.py    --root $R               # class-aware split BY VIDEO
ds_pack.py     --root $R               # assemble COCO, re-assert invariants
ds_stats.py    --root $R               # class + box-size histograms
ds_manifest.py --root $R --version X.Y.Z
ds_validate.py --root $R               # the gate — must pass before training use
```

Prove the whole non-GPU spine with no network/GPU/ffmpeg:
```
ds_make_fixture.py --root /tmp/fx
ds_merge.py; ds_merge.py; ds_split.py; ds_pack.py --pending drop; \
  ds_stats.py; ds_manifest.py --version 0.0.1-fixture; ds_validate.py   # all --root /tmp/fx
pytest tools/tests
```

## Pitfalls

> **Split by video, never by frame.** Frames of one strike are near-duplicates;
> a frame-level split leaks val/test into train and inflates every metric.
> `ds_split.py` splits on `source_video_id` and `ds_validate.py` proves no leak.

> **10–40 px labels are the least trustworthy and the most important.** That
> band is the detector's crux and the models' hardest case — it's why the review
> queue ranks small boxes first. Trust `stats.md`'s crux-band counts over the
> raw totals.

> **The catalog is a moving target.** Videos are added/renamed over time. Pin
> the commit (the snapshot does) and re-fetch fresh for each build; don't assume
> a fixed count.

> **Not our camera geometry.** Assorted 720p/1080p top-down FPV footage gives
> scale diversity + the closing dive, not the seeker's calibrated 1440×1088 /
> 300–500 m DRI. Boxes are stored in absolute source px; `ds_stats` also reports
> them seeker-referenced (rescaled to 1088 rows), labelled as such.

> **Rights are a real go/no-go.** Catalog text is CC0; the **video pixels carry
> no license** and the content is sensitive. `videos/` is a quarantined mirror —
> research/POC only, never redistributed. See `reports/inventory.md`.

## Layout

| path | role |
|---|---|
| `SPEC.md` | the frozen recipe (`layout-1`) — layout + label rules |
| `schema/` | JSON Schemas + frozen category/synonym tables (single source of truth) |
| `tools/airpoc_ds/` | pure, unit-tested helpers (luma, dedup, terminal, coco, …) |
| `tools/ds_*.py` | the pipeline stages (one responsibility each) |
| `tools/tests/` | pytest — helpers + full fixture spine |
| `review_app/` | the human-review web app (stdlib server + plain HTML/JS) |
