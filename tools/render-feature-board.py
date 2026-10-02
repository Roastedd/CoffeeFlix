#!/usr/bin/env python3
"""Render static voting cards from the catalogue; no build needed to serve the site."""
import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_URL = "https://github.com/Roastedd/CoffeeFlix"
STATUSES = {
    "proposed": "Proposed", "needs-research": "Needs research", "planned": "Planned",
    "in-progress": "In progress", "shipped": "Shipped", "not-planned": "Not planned",
}
esc = lambda value: html.escape(str(value), quote=True)


def load_catalogue():
    items = json.loads((ROOT / "docs/assets/feature-requests.json").read_text())
    seen = set()
    for item in items:
        assert re.fullmatch(r"[a-z0-9-]+", item["id"]), f"Invalid feature id {item['id']!r}"
        assert item["id"] not in seen, f"Duplicate feature id {item['id']!r}"
        assert item["status"] in STATUSES, f"Unknown status {item['status']!r}"
        assert all(type(n) is int and n > 0 for n in item.get("issues", [])), "Issue numbers must be positive integers"
        seen.add(item["id"])
    return items


def source_html(item):
    parts = [esc(item["source"])]
    parts += [f'<a href="{REPO_URL}/issues/{n}">GitHub #{n}</a>' for n in item.get("issues", [])]
    return " · ".join(parts)


def card_html(item, thread):
    includes = ""
    if item.get("includes"):
        includes = '\n            <ul class="includes">' + "".join(f"<li>{esc(line)}</li>" for line in item["includes"]) + "</ul>"
    return f'''          <article class="card" id="{item['id']}" data-category="{esc(item['category'])}" data-status="{item['status']}" aria-labelledby="title-{item['id']}">
            <div class="card-head"><span class="category">{esc(item['category'])}</span><span class="badge" data-state="{item['status']}">{STATUSES[item['status']]}</span></div>
            <h3 id="title-{item['id']}">{esc(item['title'])}</h3>
            <p class="summary">{esc(item['summary'])}</p>{includes}
            <p class="source">{source_html(item)}</p>
            <div class="card-foot">
              <p class="tally"><strong data-votes aria-label="Vote count unavailable">—</strong> <span data-vote-label>votes</span></p>
              <a class="vote-btn" href="{thread}" target="_blank" rel="noopener" aria-label="Vote for {esc(item['title'])} on GitHub"><svg aria-hidden="true"><use href="#i-thumb"/></svg>Vote on GitHub<span aria-hidden="true"> ↗</span></a>
            </div>
            <div class="meter" aria-hidden="true"><i></i></div>
          </article>'''


def pills_html(items):
    categories = list(dict.fromkeys(item["category"] for item in items))
    pills = [f'<button class="pill" type="button" data-category="all" aria-pressed="true">All <span>{len(items)}</span></button>']
    for category in categories:
        count = sum(item["category"] == category for item in items)
        pills.append(f'<button class="pill" type="button" data-category="{esc(category)}" aria-pressed="false">{esc(category)} <span>{count}</span></button>')
    return "\n            ".join(pills)


def replace_region(page, name, body):
    page, count = re.subn(rf"(<!-- BEGIN {name} -->).*?(<!-- END {name} -->)",
                          lambda m: f"{m.group(1)}\n{body}\n{' ' * 10}{m.group(2)}", page, flags=re.S)
    assert count == 1, f"Missing or duplicate {name} markers"
    return page


def render():
    items = load_catalogue()
    issue = json.loads((ROOT / "docs/assets/feature-thread.json").read_text())["issue"]
    assert type(issue) is int and issue > 0, "Invalid voting thread number"
    thread = f"{REPO_URL}/issues/{issue}"
    path = ROOT / "docs/features.html"
    page = path.read_text()
    page = replace_region(page, "FEATURE CARDS", "\n".join(card_html(item, thread) for item in items))
    page = replace_region(page, "CATEGORY PILLS", "            " + pills_html(items))
    page, count = re.subn(r'(<div class="feature-grid" id="feature-grid" data-thread=")\d+(")', rf"\g<1>{issue}\g<2>", page)
    assert count == 1, "Missing feature grid"
    page, count = re.subn(rf'href="{re.escape(REPO_URL)}/issues/\d+"( data-thread-link)', f'href="{thread}"\\1', page)
    assert count >= 2, "Expected the how-to and footer voting thread links"
    page = re.sub(r"(<[a-z]+ data-idea-count>)\d+(</)", rf"\g<1>{len(items)}\g<2>", page)
    path.write_text(page)
    print(f"Rendered {len(items)} feature cards.")


if __name__ == "__main__":
    render()
