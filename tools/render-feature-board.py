#!/usr/bin/env python3
"""Render static voting cards from the catalogue; no build needed to serve the site."""
import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATUSES = {
    "proposed": "Proposed", "needs-research": "Needs research", "planned": "Planned",
    "in-progress": "In progress", "shipped": "Shipped", "not-planned": "Not planned",
}


def render():
    entries = json.loads((ROOT / "docs/assets/feature-requests.json").read_text())
    cards = []
    seen_ids, seen_comments = set(), set()
    esc = lambda value: html.escape(str(value), quote=True)
    for item in entries:
        assert re.fullmatch(r"[a-z0-9-]+", item["id"]), "Invalid feature id"
        assert item["id"] not in seen_ids, "Duplicate feature id"
        assert type(item["commentId"]) is int and item["commentId"] > 0, "Invalid comment id"
        assert item["commentId"] not in seen_comments, "Duplicate voting comment"
        seen_ids.add(item["id"])
        seen_comments.add(item["commentId"])
        status = STATUSES[item["status"]]
        source = esc(item["source"])
        if item.get("sourceUrl"):
            assert item["sourceUrl"].startswith("https://github.com/Roastedd/CoffeeFlix/issues/"), "Invalid source link"
            source = f'<a href="{esc(item["sourceUrl"])}">{source} ↗</a>'
        url = f'https://github.com/Roastedd/CoffeeFlix/issues/15#issuecomment-{item["commentId"]}'
        cards.append(f'''          <article class="feature-card" id="{item['id']}" data-comment-id="{item['commentId']}" data-category="{esc(item['category'])}" data-status="{item['status']}" aria-labelledby="title-{item['id']}">
            <div class="card-top"><span class="category">{esc(item['category'])}</span><span class="badge" data-state="{item['status']}">{status}</span></div>
            <h3 id="title-{item['id']}">{esc(item['title'])}</h3>
            <p class="summary">{esc(item['summary'])}</p>
            <p class="source">{source}</p>
            <div class="card-bottom"><span class="vote-total"><span class="thumb" aria-hidden="true">👍</span><strong data-votes aria-label="Vote count unavailable">—</strong> <span data-vote-label>votes</span></span><a class="vote-link" href="{url}" aria-label="Vote on GitHub for {esc(item['title'])}">Vote on GitHub ↗</a></div>
          </article>''')
    path = ROOT / "docs/features.html"
    page = path.read_text()
    page, replacements = re.subn(
        r"<!-- BEGIN FEATURE CARDS -->.*?<!-- END FEATURE CARDS -->",
        "<!-- BEGIN FEATURE CARDS -->\n" + "\n".join(cards) + "\n          <!-- END FEATURE CARDS -->",
        page, flags=re.S,
    )
    assert replacements == 1, "Missing or duplicate card markers"
    page = re.sub(r'(<span class="idea-count">)\d+( ideas</span>)', rf'\g<1>{len(entries)}\g<2>', page)
    page = re.sub(r'(<p id="results" role="status">)\d+( community ideas</p>)', rf'\g<1>{len(entries)}\g<2>', page)
    path.write_text(page)
    print(f"Rendered {len(entries)} feature cards.")


if __name__ == "__main__":
    render()
