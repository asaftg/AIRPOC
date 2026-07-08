import jsonschema
import pytest

from airpoc_ds.coco import annotation_record, image_record, instances_skeleton
from airpoc_ds.review import cap_queue, toughness
from airpoc_ds.schema import categories, load, synonyms, validate

PROBE = {"width": 1920, "height": 1080, "pix_fmt": "yuv420p", "color_range": "tv"}


def test_categories_frozen():
    cats = categories()
    assert [(c["id"], c["name"]) for c in cats] == [(1, "vehicle"), (2, "human")]


def test_synonyms_cover_prompt_and_route():
    syn = synonyms()
    assert "person" in syn["map"] and syn["map"]["person"] == "human"
    assert syn["map"]["car"] == "vehicle"
    assert syn["map"]["technical"] == "REVIEW"
    assert "vehicle" in syn["gdino_prompt"] and "person" in syn["gdino_prompt"]


def test_built_coco_validates():
    coco = instances_skeleton("train")
    img = image_record("clip_0123456789", 1500, "t", 1920, 1080, PROBE,
                       "bt709_full", "a" * 64, terminal_source="segments")
    coco["images"].append(img)
    coco["annotations"].append(annotation_record(
        img["id"], 1, [10, 20, 30, 24], 0.8, "gdino+rtmdet", "auto_ok", "car",
        {"rtmdet_iou": 0.7, "agree": True}))
    validate(coco, "coco_instances")


def test_bad_category_rejected():
    coco = instances_skeleton("train")
    img = image_record("clip_0123456789", 0, "b", 1920, 1080, PROBE,
                       "bt601_full", "b" * 64)
    coco["images"].append(img)
    ann = annotation_record(img["id"], 1, [0, 0, 5, 5], 0.5, "human", "reviewed_ok", "x")
    ann["category_id"] = 3  # illegal
    coco["annotations"].append(ann)
    with pytest.raises(jsonschema.ValidationError):
        validate(coco, "coco_instances")


def test_review_queue_schema_and_cap():
    items = []
    for i in range(5):
        it = {"image_id": i + 1, "file_name": f"v_{i}.png", "source_video_id": "v_0000000000",
              "timestamp_ms": i, "extraction_mode": "terminal",
              "reasons": ["models_disagree"],
              "boxes": [{"annotation_id": i + 1, "bbox": [0, 0, 20, 20],
                         "proposed_category_id": 1, "score": 0.3, "raw_label": "car"}],
              "suggested_action": "x"}
        it["toughness"] = toughness(it)
        items.append(it)
    kept, overflow = cap_queue(items, budget=3)
    assert len(kept) == 3 and len(overflow) == 2
    queue = {"spec_version": "layout-1", "budget": 3, "queued": len(kept),
             "overflow_dropped": len(overflow), "items": kept}
    validate(queue, "review_queue")


def test_manifest_schema_smoke():
    load("manifest")  # parses without error
