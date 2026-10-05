#!/usr/bin/env python3
"""Propose roadmap entries from the feature board's votes.

  propose-roadmap.py --votes votes.json [--max 3] [--min-votes 3] [--body-file body.md]

Planned ideas with the most votes join the roadmap's Planned column, up to --max in total from
this script. Priorities stay with the maintainer: ideas that are only proposed or need research are
listed in the proposal body, never added. Skipped: ideas marked "roadmap": false, Android-only
ideas (not on the public roadmap yet) and anything mentioning CoffeeUnlocked-only features.
"""
import argparse
import importlib.util
import json
import re
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = re.compile(r"\biptv\b|\bmovies?\s*/\s*tv\b|\bcoffeeunlocked\b|\bxtream\b|\bm3u\b", re.I)

_spec = importlib.util.spec_from_file_location("render_roadmap", ROOT / "tools/render-roadmap.py")
_render = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_render)


def eligible(item):
    platforms = item.get("platforms") or ["Wii U"]
    text = " ".join([item["title"], item["summary"], *item.get("includes", [])])
    return item.get("roadmap") is not False and not any("Android" in p for p in platforms) and not PRIVATE.search(text)


def propose(root, votes, limit, min_votes, today):
    """Return (list of entries added, list of ideas worth a look); edits roadmap.json when it adds."""
    road_path = root / "docs/assets/roadmap.json"
    roadmap = json.loads(road_path.read_text())
    catalogue = json.loads((root / "docs/assets/feature-requests.json").read_text())
    present = {e.get("feature_id") or e.get("id") for e in roadmap["items"]}
    count = lambda item: ((votes.get("features") or {}).get(item["id"]) or {}).get("votes") or 0
    ranked = sorted((i for i in catalogue if i["id"] not in present and eligible(i)), key=lambda i: -count(i))
    status = {i["id"]: i["status"] for i in catalogue}
    added_before = sum(1 for e in roadmap["items"] if e.get("auto_votes") and status.get(e.get("feature_id")) == "planned")
    slots = max(0, limit - added_before)
    planned = [i for i in ranked if i["status"] == "planned" and count(i) >= min_votes][:slots]
    watch = [i for i in ranked if i["status"] in ("proposed", "needs-research") and count(i) >= min_votes][:3]
    for item in planned:
        roadmap["items"].append({"feature_id": item["id"], "platforms": item.get("platforms") or ["Wii U"], "auto_votes": True})
    if planned:
        roadmap["updated"] = today.isoformat()
        road_path.write_text(json.dumps(roadmap, indent=2, ensure_ascii=False) + "\n")
        _render.render(root)
    return [(i, count(i)) for i in planned], [(i, count(i)) for i in watch]


def body(added, watch):
    lines = ["Weekly roadmap proposal from the feature board's 👍 votes.", ""]
    if added:
        lines += ["**Added to Planned**", *[f"- {i['title']} (`{i['id']}`): {n} votes" for i, n in added], ""]
    if watch:
        lines += ["**Popular, but not yet planned (your call: change their status in `feature-requests.json` to add them)**",
                  *[f"- {i['title']} (`{i['id']}`, {i['status']}): {n} votes" for i, n in watch], ""]
    lines.append("Merge to publish, or close to skip this week. Nothing changes until this is merged.")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--votes", required=True)
    parser.add_argument("--max", type=int, default=3)
    parser.add_argument("--min-votes", type=int, default=3)
    parser.add_argument("--body-file")
    args = parser.parse_args()
    votes = json.loads(Path(args.votes).read_text())
    added, watch = propose(ROOT, votes, args.max, args.min_votes, date.today())
    text = body(added, watch)
    print(text)
    if args.body_file and (added or watch):
        Path(args.body_file).write_text(text)
