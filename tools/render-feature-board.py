#!/usr/bin/env python3
"""Render static voting cards from the catalogue; no build needed to serve the site."""
import hashlib
import html
import importlib.util
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


def validate_catalogue(items):
    seen = set()
    for item in items:
        assert re.fullmatch(r"[a-z0-9-]+", item["id"]), f"Invalid feature id {item['id']!r}"
        assert item["id"] not in seen, f"Duplicate feature id {item['id']!r}"
        for key in ("category", "title", "summary"):
            assert isinstance(item.get(key), str) and item[key].strip(), f"{item['id']}: {key} can't be empty"
        assert item["status"] in STATUSES, f"Unknown status {item['status']!r}"
        assert isinstance(item.get("reply", ""), str), "reply must be text"
        version = item.get("shipped_in", "")
        assert isinstance(version, str) and (not version or re.fullmatch(r"\d+\.\d+(\.\d+)?", version)), f"{item['id']}: shipped_in must be a version like 2.4.0"
        assert not version or item["status"] == "shipped", f"{item['id']}: only shipped ideas can say which version shipped them"
        assert all(isinstance(line, str) for line in item.get("includes", [])), "includes must be a list of text lines"
        assert all(type(n) is int and n > 0 for n in item.get("issues", [])), "Issue numbers must be positive integers"
        seen.add(item["id"])
    return items


def load_catalogue(root=ROOT):
    return validate_catalogue(json.loads((root / "docs/assets/feature-requests.json").read_text()))


def source_html(item):
    parts = [esc(item["source"])] if item.get("source") else []
    parts += [f'<a href="{REPO_URL}/issues/{n}">GitHub #{n}</a>' for n in item.get("issues", [])]
    return " · ".join(parts)


def card_html(item, thread):
    includes = ""
    if item.get("includes"):
        includes = '\n            <ul class="includes">' + "".join(f"<li>{esc(line)}</li>" for line in item["includes"]) + "</ul>"
    source = f'\n            <p class="source">{source_html(item)}</p>' if item.get("source") or item.get("issues") else ""
    reply = ""
    if item.get("reply"):
        reply = f'\n            <p class="reply"><b>Dev reply</b> {esc(item["reply"])}</p>'
    version = item.get("shipped_in", "")
    release = f"{REPO_URL}/releases/tag/v{version}"
    badge = (f'<a class="badge" data-state="shipped" href="{release}" target="_blank" rel="noopener">Shipped in v{esc(version)}</a>' if version
             else f'<span class="badge" data-state="{item["status"]}">{STATUSES[item["status"]]}</span>')
    # A shipped idea doesn't take votes any more: the card points to what it became.
    action = (f'<a class="release-btn" href="{release}" target="_blank" rel="noopener" aria-label="Read what is new in version {esc(version)} on GitHub"><svg aria-hidden="true"><use href="#i-check"/></svg>What’s new<span aria-hidden="true"> ↗</span></a>' if version
              else f'<a class="vote-btn" href="{thread}" target="_blank" rel="noopener" aria-label="Vote for {esc(item["title"])} on GitHub"><svg aria-hidden="true"><use href="#i-thumb"/></svg>Vote on GitHub<span aria-hidden="true"> ↗</span></a>')
    return f'''          <article class="card" id="{item['id']}" data-category="{esc(item['category'])}" data-status="{item['status']}" aria-labelledby="title-{item['id']}">
            <div class="card-head"><span class="category">{esc(item['category'])}</span>{badge}</div>
            <h3 id="title-{item['id']}">{esc(item['title'])}</h3>
            <p class="summary">{esc(item['summary'])}</p>{includes}{reply}{source}
            <div class="card-foot">
              <p class="tally"><strong data-votes aria-label="Vote count unavailable">—</strong> <span data-vote-label>votes</span></p>
              {action}
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


def just_shipped_html(release):
    """The "Just shipped" block from docs/assets/latest-release.json (tools/update-just-shipped.py writes it)."""
    version = release["version"]
    assert re.fullmatch(r"\d+\.\d+(\.\d+)?", version), f"Bad version in latest-release.json: {version}"
    url = release.get("url") or f"{REPO_URL}/releases/tag/v{version}"
    assert url.startswith(REPO_URL + "/releases/"), "The release link must point at this repository's releases"
    lines = [
        '      <section class="update" aria-labelledby="update-title">',
        '        <div class="update-head">',
        '          <div>',
        '            <span class="eyebrow">Just shipped</span>',
        f'            <h2 id="update-title">CoffeeFlix {esc(version)} is out</h2>',
        '          </div>',
        f'          <a class="btn small" href="{esc(url)}" target="_blank" rel="noopener">Release notes <span aria-hidden="true">↗</span></a>',
        '        </div>',
    ]
    highlights = [h for h in release.get("highlights", []) if h.strip()]
    if highlights:
        lines.append('        <ul class="update-list">')
        lines += [f"          <li>{esc(h)}</li>" for h in highlights]
        lines.append("        </ul>")
    if release.get("note"):
        lines.append(f'        <p class="update-note">{esc(release["note"])}</p>')
    lines.append("      </section>")
    return "\n".join(lines)


def shipped_html(items):
    """Ideas that shipped, as one short line each under the grid (the grid shows ideas still to come)."""
    shipped = [i for i in items if i["status"] == "shipped"]
    if not shipped:
        return ""
    def key(i):
        return tuple(int(x) for x in i.get("shipped_in", "0").split(".")), i["title"]
    rows = []
    for item in sorted(shipped, key=key, reverse=True):
        version = item.get("shipped_in")
        where = (f'<a href="{REPO_URL}/releases/tag/v{esc(version)}" target="_blank" rel="noopener">v{esc(version)}</a>'
                 if version else "")
        rows.append(f'            <li><a class="shipped-title" href="#{esc(item["id"])}" data-show-card="{esc(item["id"])}">{esc(item["title"])}</a>'
                    f'<span class="shipped-in">{where}</span></li>')
    return "\n".join([
        '        <section class="shipped" id="shipped" aria-labelledby="shipped-title">',
        f'          <h3 id="shipped-title"><span class="eyebrow">Done</span> Shipped from this board <span class="shipped-count">{len(shipped)}</span></h3>',
        '          <ul class="shipped-list">', *rows, '          </ul>',
        '        </section>'])


def render(root=ROOT):
    items = load_catalogue(root)
    issue = json.loads((root / "docs/assets/feature-thread.json").read_text())["issue"]
    assert type(issue) is int and issue > 0, "Invalid voting thread number"
    thread = f"{REPO_URL}/issues/{issue}"
    path = root / "docs/features.html"
    page = path.read_text()
    page = replace_region(page, "FEATURE CARDS", "\n".join(card_html(item, thread) for item in items))
    page = replace_region(page, "CATEGORY PILLS", "            " + pills_html(items))
    page = replace_region(page, "SHIPPED LIST", shipped_html(items))
    # The page's own script and styles carry a hash of their contents, so a visitor never runs a cached older script
    # against newer markup (GitHub Pages caches for ten minutes).
    for asset in ("features.js", "features.css"):
        if not (root / "docs/assets" / asset).exists():
            continue
        digest = hashlib.sha256((root / "docs/assets" / asset).read_bytes()).hexdigest()[:10]
        page = re.sub(rf'assets/{re.escape(asset)}(\?v=[0-9a-f]+)?"', f'assets/{asset}?v={digest}"', page)
    latest = root / "docs/assets/latest-release.json"
    if latest.exists():
        page = replace_region(page, "JUST SHIPPED", just_shipped_html(json.loads(latest.read_text())))
    page, count = re.subn(r'(<div class="feature-grid" id="feature-grid" data-thread=")\d+(")', rf"\g<1>{issue}\g<2>", page)
    assert count == 1, "Missing feature grid"
    page, count = re.subn(rf'href="{re.escape(REPO_URL)}/issues/\d+"( data-thread-link)', f'href="{thread}"\\1', page)
    assert count >= 2, "Expected the how-to and footer voting thread links"
    page = re.sub(r"(<[a-z]+ data-idea-count>)\d+(</)", rf"\g<1>{len(items)}\g<2>", page)
    roadmap_spec = importlib.util.spec_from_file_location("render_roadmap", Path(__file__).with_name("render-roadmap.py"))
    roadmap = importlib.util.module_from_spec(roadmap_spec)
    roadmap_spec.loader.exec_module(roadmap)
    roadmap.render(root)
    path.write_text(page)
    print(f"Rendered {len(items)} feature cards.")


if __name__ == "__main__":
    render()
