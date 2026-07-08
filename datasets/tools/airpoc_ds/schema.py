"""Load + validate against the frozen JSON Schemas in datasets/schema/."""

import json
from functools import lru_cache
from pathlib import Path

import jsonschema

SCHEMA_DIR = Path(__file__).resolve().parents[2] / "schema"


@lru_cache(maxsize=None)
def load(name: str) -> dict:
    """load('manifest') -> parsed datasets/schema/manifest.schema.json"""
    return json.loads((SCHEMA_DIR / f"{name}.schema.json").read_text(encoding="utf-8"))


def validate(obj, schema_name: str):
    """Raises jsonschema.ValidationError with a useful path on failure."""
    jsonschema.validate(obj, load(schema_name))


def synonyms() -> dict:
    return json.loads((SCHEMA_DIR / "class_synonyms.json").read_text(encoding="utf-8"))


def categories() -> list:
    return json.loads((SCHEMA_DIR / "categories.json").read_text(encoding="utf-8"))
