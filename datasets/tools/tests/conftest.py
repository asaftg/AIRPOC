import sys
from pathlib import Path

# make the airpoc_ds package importable from tools/
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
