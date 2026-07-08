"""airpoc_ds — pure helpers for the datasets/ pipeline tools.

Everything here is offline bench tooling (Python allowed per the engineering
guidelines). No GPU, no network. The GPU/network work lives in the ds_* tools
that import this package.
"""

import os
import sys
from pathlib import Path

SPEC_VERSION = "layout-1"

CATALOG_REPO = "https://github.com/itamarwe/fpv-drone-strikes-lebanon-dataset.git"

# frozen extraction defaults (SPEC.md §4) — deviations are recorded in the manifest
BASELINE_FPS = 2.0
TERMINAL_TAIL_S = 6.0
TERMINAL_FPS = 15.0
DEDUP_HAMMING_BASELINE = 4
# Terminal dedup is DISABLED (None = keep every frame). dHash is deliberately
# scale-tolerant, so it cannot safely tell a growing target apart from a repeat
# — any dedup risks decimating the closing dive, the exact sweep this dataset
# exists to capture. The sweep is small (~tail_s x terminal_fps frames) and
# precious, so it is kept whole. Redundancy is only a problem in baseline mode
# (2 fps over minutes of flight/replay), which still dedups.
DEDUP_HAMMING_TERMINAL = None
DEDUP_WINDOW_S = 2.0

# frozen merge defaults (SPEC.md §5)
AUTO_ACCEPT_SCORE = 0.45
CROSS_CHECK_IOU = 0.5
REVIEW_BUDGET_DEFAULT = 200

USE_RESTRICTION = (
    "Catalog text is CC0; the source videos are unlicensed third-party "
    "sensitive media. Research/POC use only, quarantined on the dataset "
    "drive, never redistributed."
)


def dataset_root(cli_value=None):
    """Resolve DATASET_ROOT from --root or the environment. Exits with a clear
    message if neither is set — there is no default, the data lives off-repo."""
    value = cli_value or os.environ.get("DATASET_ROOT")
    if not value:
        sys.exit("DATASET_ROOT not set: pass --root or export DATASET_ROOT "
                 "(the dataset lives on the external drive, never in the repo)")
    return Path(value)


def ensure_layout(root: Path):
    """Create the DATASET_ROOT directory skeleton (idempotent)."""
    for sub in ("catalog", "videos", "frames", "raw_labels", "annotations",
                "splits", "review", "exclusions", "reports", "logs"):
        (root / sub).mkdir(parents=True, exist_ok=True)
    return root


def write_json(path, obj):
    """Deterministic JSON write (atomic: tmp + rename) so content hashes are stable."""
    import json
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=1, ensure_ascii=False, sort_keys=True) + "\n",
                   encoding="utf-8")
    tmp.replace(path)


def read_json(path):
    import json
    return json.loads(Path(path).read_text(encoding="utf-8"))
