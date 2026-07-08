"""Review-queue ranking + budget cap (SPEC.md §5).

The human sees at most `budget` boxes, the toughest/most valuable first.
Overflow is dropped and ledgered (REVIEW_OVERFLOW_DROP) — never silently,
never handed to the human anyway.
"""


def toughness(item):
    """Value score for a review item. Higher = shown first.

    10-40 px boxes are the detector's crux band; terminal frames are the
    scale-closing sweep; disagreement and low confidence mean the models
    genuinely don't know.
    """
    score = 0.0
    sides = [max(b["bbox"][2], b["bbox"][3]) for b in item["boxes"]] or [0.0]
    if any(10.0 <= s <= 40.0 for s in sides):
        score += 2.0
    elif any(s < 10.0 for s in sides):
        score += 1.0
    if item["extraction_mode"] == "terminal":
        score += 1.0
    reasons = set(item["reasons"])
    if "models_disagree" in reasons:
        score += 1.0
    if "terminal_no_labels" in reasons:
        score += 1.5
    if "crowded" in reasons:
        score += 0.5
    confs = [b["score"] for b in item["boxes"]]
    if confs:
        score += max(0.0, 0.45 - min(confs))
    return round(score, 4)


def cap_queue(items, budget):
    """-> (kept, overflow), both sorted by toughness desc, deterministic."""
    ranked = sorted(items, key=lambda i: (-i["toughness"], i["file_name"], i["image_id"]))
    return ranked[:budget], ranked[budget:]
