#!/usr/bin/env python3
"""Fill the feature board's "Just shipped" block from a published release.

  update-just-shipped.py [VERSION] [--body-file FILE] [--url URL]

Without VERSION it takes the newest published release. The highlights are the release notes' bullet
points (the first five, after the opening sentence); the note under them names the ideas from the board that the version brought
(release/features.json, and any card marked shipped in it), or simply thanks the voters. Writes
docs/assets/latest-release.json and re-renders docs/features.html. With --body-file nothing is fetched
(for tests, or a release that isn't on GitHub yet). GITHUB_TOKEN or GH_TOKEN is used if set.
"""
import argparse
import importlib.util
import json
import os
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
API = "https://api.github.com/repos/Roastedd/CoffeeFlix/releases"
MAX_HIGHLIGHTS = 5


def fetch(version=None):
    url = f"{API}/tags/v{version}" if version else f"{API}/latest"
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "coffeeflix-board"})
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=30) as response:
        release = json.load(response)
    if release.get("draft"):
        raise SystemExit(f"{release.get('tag_name')} is still a draft")
    return release["tag_name"].lstrip("v"), release.get("body") or "", release.get("html_url")


def plain(text):
    """Markdown to the plain text the page shows: links keep their words, emphasis and code marks go."""
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"(\*\*|__|\*|_|`)(.+?)\1", r"\2", text)
    return re.sub(r"\s+", " ", text).strip()


def lead(body):
    """The release's opening sentence, when the notes open with a paragraph before the list."""
    first = next((p for p in re.split(r"\n\s*\n", body.strip()) if p.strip()), "")
    if not first or re.match(r"\s{0,3}([-*+]|#)", first):
        return ""
    sentence = re.split(r"(?<=[.!?])\s", plain(first), maxsplit=1)[0]
    return sentence if len(sentence) <= 160 else ""


def highlights(body):
    out = [lead(body)] if lead(body) else []
    for line in body.splitlines():
        m = re.match(r"\s{0,3}[-*+]\s+(.+)", line)
        if m and plain(m.group(1)):
            out.append(plain(m.group(1)))
        if len(out) == MAX_HIGHLIGHTS:
            break
    if len(out) <= 1 and not any(re.match(r"\s{0,3}[-*+]\s", l) for l in body.splitlines()):  # no list: the first paragraph, cut to a sentence or two
        para = next((plain(p) for p in re.split(r"\n\s*\n", body) if plain(p)), "")
        if para and not out:
            out.append(para if len(para) <= 200 else para[:197].rsplit(" ", 1)[0] + "…")
    return out


def from_board(root, version):
    """Titles of the board's ideas (and roadmap items) that this version brought."""
    catalogue = json.loads((root / "docs/assets/feature-requests.json").read_text())
    titles = {i["id"]: i["title"] for i in catalogue}
    roadmap = root / "docs/assets/roadmap.json"
    if roadmap.exists():
        titles.update({e["id"]: e["title"] for e in json.loads(roadmap.read_text())["items"] if "id" in e and "title" in e})
    features = root / "release/features.json"
    ids = json.loads(features.read_text()).get(version, []) if features.exists() else []
    ids += [i["id"] for i in catalogue if i.get("status") == "shipped" and i.get("shipped_in") == version and i["id"] not in ids]
    return [titles[i] for i in ids if i in titles]


def note(names):
    if not names:
        return "Thanks for voting, and keep the ideas coming."
    listed = names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]
    return f"From this board: {listed}. Thanks for voting, and keep the ideas coming."


def update(root, version, body, url=None):
    release = {"version": version, "url": url or f"https://github.com/Roastedd/CoffeeFlix/releases/tag/v{version}",
               "highlights": highlights(body), "note": note(from_board(root, version))}
    path = root / "docs/assets/latest-release.json"
    old = json.loads(path.read_text()) if path.exists() else None
    if old and old.get("version") != version and tuple(map(int, old["version"].split("."))) > tuple(map(int, version.split("."))):
        raise SystemExit(f"Not going back: the board shows {old['version']}, newer than {version}")
    path.write_text(json.dumps(release, indent=2, ensure_ascii=False) + "\n")
    spec = importlib.util.spec_from_file_location("board", root / "tools/render-feature-board.py")
    board = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(board)
    board.render(root)
    return release


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?")
    parser.add_argument("--body-file")
    parser.add_argument("--url")
    args = parser.parse_args()
    if args.body_file:
        if not args.version:
            raise SystemExit("--body-file needs the version")
        version, body, url = args.version.lstrip("v"), Path(args.body_file).read_text(), args.url
    else:
        version, body, url = fetch(args.version.lstrip("v") if args.version else None)
    print(json.dumps(update(ROOT, version, body, url), indent=2, ensure_ascii=False))
