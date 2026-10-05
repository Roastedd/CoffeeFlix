#!/usr/bin/env python3
"""propose-roadmap.py adds popular planned ideas, never proposed ones, and skips ineligible ones."""
import importlib.util
import json
import shutil
import tempfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("propose", ROOT / "tools/propose-roadmap.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def idea(i, status, **extra):
    return {"id": i, "category": "Test", "title": f"Idea {i}", "summary": "A small improvement.", "status": status, **extra}


with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    (root / "docs/assets").mkdir(parents=True)
    (root / "tools").mkdir()
    shutil.copy(ROOT / "docs/index.html", root / "docs/index.html")
    catalogue = [idea("a", "planned"), idea("b", "planned"), idea("c", "planned", roadmap=False),
                 idea("d", "planned", platforms=["Android"]), idea("e", "proposed"),
                 idea("f", "planned", summary="Better IPTV lists."), idea("g", "planned"), idea("h", "planned")]
    (root / "docs/assets/feature-requests.json").write_text(json.dumps(catalogue))
    (root / "docs/assets/roadmap.json").write_text(json.dumps({"updated": "2026-01-01", "items": []}))
    votes = {"features": {k: {"votes": v} for k, v in dict(a=9, b=5, c=50, d=50, e=40, f=50, g=4, h=1).items()}}
    added, watch = mod.propose(root, votes, 2, 3, date(2026, 10, 12))
    assert [i["id"] for i, _ in added] == ["a", "b"], added          # most votes first, only 2, nothing ineligible
    assert [i["id"] for i, _ in watch] == ["e"], watch                 # proposed: mentioned, never added
    road = json.loads((root / "docs/assets/roadmap.json").read_text())
    assert road["updated"] == "2026-10-12" and all(e["auto_votes"] for e in road["items"])
    assert mod.propose(root, votes, 2, 3, date(2026, 10, 19))[0] == []   # the cap counts earlier additions
print("propose-roadmap ok")
