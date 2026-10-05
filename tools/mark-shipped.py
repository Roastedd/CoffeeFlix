#!/usr/bin/env python3
"""Move features to Released when a version goes public.

  mark-shipped.py VERSION ID [ID ...] [--keep 6]

Each idea becomes "shipped" in version VERSION on the feature board (the voting thread's comment
follows on the next sync), and appears in the roadmap's Released column. The Released column keeps
the newest --keep cards; older ones leave the roadmap but stay on the feature board as shipped.
"""
import argparse
import importlib.util
import json
import re
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("render_roadmap", ROOT / "tools/render-roadmap.py")
_render = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_render)


def mark(root, version, ids, keep, today):
    if not re.fullmatch(r"\d+\.\d+(\.\d+)?", version):
        raise SystemExit(f"Bad version: {version}")
    cat_path, road_path = root / "docs/assets/feature-requests.json", root / "docs/assets/roadmap.json"
    catalogue, roadmap = json.loads(cat_path.read_text()), json.loads(road_path.read_text())
    by_id = {i["id"]: i for i in catalogue}
    inline = {e["id"]: e for e in roadmap["items"] if "id" in e}  # roadmap-only items
    missing = [i for i in ids if i not in by_id and i not in inline]
    if missing:
        raise SystemExit(f"Not on the feature board or roadmap: {', '.join(missing)}")
    on_road = {e.get("feature_id") for e in roadmap["items"]}
    for ident in ids:
        target = by_id.get(ident) or inline[ident]
        target["status"], target["shipped_in"] = "shipped", version
        if ident in by_id and ident not in on_road:
            roadmap["items"].append({"feature_id": ident, "platforms": by_id[ident].get("platforms") or ["Wii U"]})
    shipped = [e for e in roadmap["items"]
               if (by_id[e["feature_id"]]["status"] if "feature_id" in e else e.get("status")) == "shipped"]
    for old in shipped[:-keep] if keep and len(shipped) > keep else []:
        roadmap["items"].remove(old)
    roadmap["updated"] = today.isoformat()
    cat_path.write_text(json.dumps(catalogue, indent=2, ensure_ascii=False) + "\n")
    road_path.write_text(json.dumps(roadmap, indent=2, ensure_ascii=False) + "\n")
    _render.render(root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    parser.add_argument("ids", nargs="+")
    parser.add_argument("--keep", type=int, default=6)
    args = parser.parse_args()
    mark(ROOT, args.version, args.ids, args.keep, date.today())
