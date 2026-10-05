#!/usr/bin/env python3
"""Render the homepage roadmap, reusing feature-board entries by ID."""
import argparse
from datetime import date
import html
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
STAGES = {"in-progress": "In progress", "in-testing": "In testing", "planned": "Planned", "shipped": "Released"}
PLATFORMS = {"Wii U", "Android", "Android phone", "Android TV", "Fire TV"}
esc = lambda value: html.escape(str(value), quote=True)


def render(root=ROOT, check=False):
    data = json.loads((root / "docs/assets/roadmap.json").read_text())
    updated = date.fromisoformat(data["updated"])
    features = {item["id"]: item for item in json.loads((root / "docs/assets/feature-requests.json").read_text())}
    groups = {stage: [] for stage in STAGES}
    seen = set()
    for entry in data["items"]:
        item = features.get(entry["feature_id"]) if "feature_id" in entry else entry
        # Removed or reconsidered voting-board ideas leave the committed roadmap.
        if item is None or ("feature_id" in entry and item["status"] not in STAGES):
            continue
        ident = item["id"]
        if not re.fullmatch(r"[a-z0-9-]+", ident) or ident in seen:
            raise ValueError(f"Invalid or duplicate roadmap ID: {ident}")
        seen.add(ident)
        if item["status"] not in STAGES:
            raise ValueError(f"Unsupported roadmap status: {item['status']}")
        if not entry["platforms"] or any(p not in PLATFORMS for p in entry["platforms"]):
            raise ValueError(f"Invalid platforms: {ident}")
        platforms = " · ".join(entry["platforms"])
        link = f'<a href="features.html#{esc(ident)}">Feature details <span aria-hidden="true">↗</span></a>' if "feature_id" in entry else ""
        version = item.get("shipped_in")
        if version:
            if item["status"] != "shipped" or not re.fullmatch(r"\d+\.\d+(\.\d+)?", version):
                raise ValueError(f"Invalid released version: {ident}")
            link = f'<a href="https://github.com/Roastedd/CoffeeFlix/releases/tag/v{version}">Released in {version} <span aria-hidden="true">↗</span></a>'
        groups[item["status"]].append(f'''          <article class="roadmap-card" aria-labelledby="roadmap-{ident}">
            <span class="roadmap-platform">{esc(platforms)}</span>
            <h4 id="roadmap-{ident}">{esc(item['title'])}</h4>
            <p>{esc(item['summary'])}</p>
            {link}
          </article>''')
    columns = []
    for stage, label in STAGES.items():
        cards = "\n".join(groups[stage]) or '<p class="roadmap-empty">No items at this stage right now.</p>'
        columns.append(f'''        <div class="roadmap-column" data-stage="{stage}">
          <h3><span class="roadmap-dot" aria-hidden="true"></span>{label}</h3>
{cards}
        </div>''')
    content = f'''      <p class="roadmap-updated">Roadmap reviewed <time datetime="{updated.isoformat()}">{updated.strftime('%B')} {updated.day}, {updated.year}</time></p>
      <div class="roadmap-grid">
{chr(10).join(columns)}
      </div>'''
    content = "\n".join(line.rstrip() for line in content.splitlines())
    path = root / "docs/index.html"
    before = path.read_text()
    after, count = re.subn(r"<!-- BEGIN ROADMAP -->.*?<!-- END ROADMAP -->", lambda _: f"<!-- BEGIN ROADMAP -->\n{content}\n      <!-- END ROADMAP -->", before, flags=re.S)
    if count != 1:
        raise ValueError("Expected one ROADMAP region")
    if check and before != after:
        raise ValueError("Roadmap is stale; run python3 tools/render-roadmap.py")
    if not check:
        path.write_text(after)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    render(check=args.check)
