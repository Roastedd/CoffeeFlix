#!/usr/bin/env python3
"""Show ideas as "In testing" on the roadmap while a supporter preview has them.

  mark-testing.py ID [ID ...]        put these ideas in the roadmap's In testing column
  mark-testing.py --clear ID [...]   take them back out (also done by mark-shipped.py when they ship)

Only ideas that are In progress on the feature board move; the board's own status doesn't change. The roadmap's
"reviewed" date is set to today.
"""
import argparse
import json
from datetime import date
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("render_roadmap", ROOT / "tools/render-roadmap.py")
_render = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_render)


def mark(root, ids, clear, today):
    cat = {i["id"]: i for i in json.loads((root / "docs/assets/feature-requests.json").read_text())}
    road_path = root / "docs/assets/roadmap.json"
    roadmap = json.loads(road_path.read_text())
    entries = {e.get("feature_id"): e for e in roadmap["items"]}
    for ident in ids:
        if ident not in cat:
            raise SystemExit(f"Not on the feature board: {ident}")
        entry = entries.get(ident)
        if entry is None:
            if clear:
                continue
            entry = {"feature_id": ident, "platforms": cat[ident].get("platforms") or ["Wii U"]}
            roadmap["items"].append(entry)
        if clear:
            entry.pop("stage", None)
        elif cat[ident]["status"] == "in-progress":
            entry["stage"] = "in-testing"
    roadmap["updated"] = today.isoformat()
    road_path.write_text(json.dumps(roadmap, indent=2, ensure_ascii=False) + "\n")
    _render.render(root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clear", action="store_true")
    parser.add_argument("ids", nargs="+")
    args = parser.parse_args()
    mark(ROOT, args.ids, args.clear, date.today())
