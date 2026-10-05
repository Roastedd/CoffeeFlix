#!/usr/bin/env python3
"""mark-shipped.py moves ideas to Released and keeps the Released column short."""
import importlib.util
import json
import shutil
import tempfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ms", ROOT / "tools/mark-shipped.py")
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

idea = lambda i, status="planned", **k: {"id": i, "category": "T", "title": f"Idea {i}", "summary": "Small.", "status": status, **k}
with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    (root / "docs/assets").mkdir(parents=True)
    shutil.copy(ROOT / "docs/index.html", root / "docs/index.html")
    (root / "docs/assets/feature-requests.json").write_text(json.dumps(
        [idea("a"), idea("b"), idea("c"), idea("old1", "shipped", shipped_in="2.4.0"), idea("old2", "shipped", shipped_in="2.4.1")]))
    (root / "docs/assets/roadmap.json").write_text(json.dumps({"updated": "2026-01-01", "items": [
        {"feature_id": "old1", "platforms": ["Wii U"]}, {"feature_id": "old2", "platforms": ["Wii U"]},
        {"feature_id": "a", "platforms": ["Wii U"]}]}))
    ms.mark(root, "2.4.4", ["a", "b"], 3, date(2026, 10, 16))
    cat = {i["id"]: i for i in json.loads((root / "docs/assets/feature-requests.json").read_text())}
    assert cat["a"]["status"] == cat["b"]["status"] == "shipped" and cat["a"]["shipped_in"] == "2.4.4"
    assert cat["c"]["status"] == "planned"
    road = json.loads((root / "docs/assets/roadmap.json").read_text())
    ids = [e["feature_id"] for e in road["items"]]
    assert ids == ["old2", "a", "b"], ids              # b added, oldest card dropped, kept 3
    assert cat["old1"]["status"] == "shipped"          # still on the feature board
    try:
        ms.mark(root, "2.4.5", ["nope"], 3, date(2026, 10, 17))
        raise AssertionError("unknown id accepted")
    except SystemExit:
        pass
print("mark-shipped ok")
