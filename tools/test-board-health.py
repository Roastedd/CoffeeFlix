#!/usr/bin/env python3
import importlib.util
from datetime import date
from pathlib import Path

spec = importlib.util.spec_from_file_location("bh", Path(__file__).resolve().parents[1] / "tools/board-health.py")
bh = importlib.util.module_from_spec(spec); spec.loader.exec_module(bh)
today = date(2026, 10, 16)
road = {"updated": "2026-10-10"}
clean = [{"id": "a", "status": "shipped", "shipped_in": "2.4.3", "issues": [1]}, {"id": "b", "status": "in-progress", "since": "2026-09-30"}]
assert bh.check(clean, road, "2.4.3", "2.4.3", set(), today) == []
assert len(bh.check(clean, road, "2.4.0", "2.4.3", set(), today)) == 1                       # page behind the release
assert len(bh.check(clean, road, "2.4.3", "2.4.3", {1}, today)) == 1                          # shipped, issue still open
assert len(bh.check([{"id": "c", "status": "shipped"}], road, "2.4.3", None, set(), today)) == 1
assert len(bh.check([{"id": "d", "status": "in-progress", "since": "2026-07-01"}], road, "2.4.3", None, set(), today)) == 1
assert len(bh.check(clean, {"updated": "2026-08-01"}, "2.4.3", None, set(), today)) == 1
print("board-health ok")
