#!/usr/bin/env python3
"""mark-testing.py moves an in-progress idea to the roadmap's In testing column, and shipping takes it out."""
import importlib.util
import json
import shutil
import tempfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"tools/{name.replace('_', '-')}.py")
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod
mt, ms = load("mark_testing"), load("mark_shipped")

idea = lambda i, status: {"id": i, "category": "T", "title": f"Idea {i}", "summary": "Small.", "status": status}
with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    (root / "docs/assets").mkdir(parents=True)
    shutil.copy(ROOT / "docs/index.html", root / "docs/index.html")
    (root / "docs/assets/feature-requests.json").write_text(json.dumps([idea("a", "in-progress"), idea("b", "planned")]))
    (root / "docs/assets/roadmap.json").write_text(json.dumps({"updated": "2026-01-01", "items": [{"feature_id": "a", "platforms": ["Wii U"]}]}))
    mt.mark(root, ["a", "b"], False, date(2026, 10, 16))
    road = json.loads((root / "docs/assets/roadmap.json").read_text())
    stages = {e["feature_id"]: e.get("stage") for e in road["items"]}
    assert stages == {"a": "in-testing", "b": None}, stages            # only an in-progress idea moves
    page = (root / "docs/index.html").read_text()
    testing = page.split('data-stage="in-testing"')[1].split('data-stage="shipped"')[0]
    assert "Idea a" in testing and "Idea b" not in testing
    mt.mark(root, ["a"], True, date(2026, 10, 17))                      # taken out again
    assert "stage" not in json.loads((root / "docs/assets/roadmap.json").read_text())["items"][0]
    mt.mark(root, ["a"], False, date(2026, 10, 17))
    ms.mark(root, "2.5.0", ["a"], 3, date(2026, 10, 18))                # shipping clears it
    entry = json.loads((root / "docs/assets/roadmap.json").read_text())["items"][0]
    assert "stage" not in entry
    cat = json.loads((root / "docs/assets/feature-requests.json").read_text())[0]
    assert cat["status"] == "shipped"
print("mark-testing ok")
