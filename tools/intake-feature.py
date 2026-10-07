#!/usr/bin/env python3
"""Turn a "Feature idea" GitHub issue into a draft card on the feature board.

  intake-feature.py --issue 17 [--title-file t.txt] [--body-file b.md]
  intake-feature.py --issue-file issue.json          (an issue as GitHub's API returns it)
  intake-feature.py --pending                        (print the numbers of open ideas not yet on the board)

Adds a "proposed" entry to docs/assets/feature-requests.json, rebuilds the board, and writes a pull
request title and body. Nothing is published until that pull request is merged, so the wording of
text written by strangers is always reviewed first. Issues that aren't feature ideas (no form fields),
that are closed, or that the catalogue already links are skipped without changing anything.
"""
import argparse
import importlib.util
import json
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = "Roastedd/CoffeeFlix"
CATALOGUE = "docs/assets/feature-requests.json"
IDEA_FIELD = "What would you like CoffeeFlix to do?"
USE_FIELD = "How would you use it?"
AREA_FIELD = "Which part of the app?"
# The issue form's "Which part of the app?" choices, as board categories.
CATEGORIES = {
    "Home and settings": "Home & settings",
    "Video playback": "Display & video",
    "YouTube": "YouTube",
    "Twitch": "Twitch",
    "Jellyfin": "Jellyfin",
    "Radio, podcasts or books": "Servers & books",
    "Something else": "Other",
}
STOP = set("a an and are as at be but by can for from have how i in into is it its me my of on or so that the this to use want with would you your coffeeflix wii".split())


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


board = _load("render_feature_board", "render-feature-board.py")
admin = _load("feature_admin", "feature-admin.py")


def api(path):
    request = urllib.request.Request(f"https://api.github.com/repos/{REPO}/{path}", headers={"Accept": "application/vnd.github+json", "User-Agent": "coffeeflix-intake"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def fields(body):
    """The answers in an issue-form body, keyed by question: '### Question', a blank line, the answer."""
    out = {}
    for part in re.split(r"(?m)^### +", body or "")[1:]:
        question, _, answer = part.partition("\n")
        answer = answer.strip()
        out[question.strip()] = "" if answer == "_No response_" else answer
    return out


def plain(text, limit):
    """One line of harmless text: no links, mentions or markup, cut at a word boundary."""
    text = re.sub(r"https?://\S+|www\.\S+", "", text or "")
    text = re.sub(r"[`*_~<>\[\]()#|\\@]", "", text)
    text = re.sub(r"[\x00-\x1f\x7f]", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) <= limit:
        return text
    return text[:limit].rsplit(" ", 1)[0].rstrip(".,;:- ") + "…"


def words(text):
    return {w for w in re.findall(r"[a-z0-9]+", text.lower()) if w not in STOP and len(w) > 2}


def similar(title, summary, items, limit=3):
    """Existing ideas sharing the most words with this one, as (score, item); a hint, never a verdict."""
    mine = words(title) | words(summary)
    ranked = []
    for item in items:
        theirs = words(" ".join([item["title"], item["summary"], *item.get("includes", [])]))
        shared = mine & theirs
        if len(shared) >= 2:
            ranked.append((len(shared), item, sorted(shared)))
    return sorted(ranked, key=lambda r: -r[0])[:limit]


def draft(issue, items):
    """(entry, why) for an issue, or (None, reason) when it isn't a new feature idea."""
    number = issue["number"]
    if "pull_request" in issue:
        return None, "it's a pull request"
    if issue.get("state") != "open":
        return None, "it's closed"
    if "enhancement" not in {label["name"] for label in issue.get("labels", [])}:
        return None, "it isn't labelled enhancement"
    if any(number in item.get("issues", []) for item in items):
        return None, "the board already links it"
    answers = fields(issue.get("body"))
    idea = plain(answers.get(IDEA_FIELD, ""), 400)
    if not idea:
        return None, "it has no idea text (not the Feature idea form)"
    title = plain(re.sub(r"(?i)^\s*feature idea:\s*", "", issue.get("title") or ""), 60) or plain(idea, 60)
    taken = {item["id"] for item in items}
    ident = admin.slug(title, taken)
    entry = {
        "id": ident,
        "category": CATEGORIES.get(answers.get(AREA_FIELD, ""), "Other"),
        "title": title,
        "summary": plain(idea, 160),
        "issues": [number],
        "status": "proposed",
    }
    return entry, ""


def pr_text(entry, issue, hints):
    use = plain(fields(issue.get("body")).get(USE_FIELD, ""), 300)
    login = issue["user"]["login"]
    lines = [
        f"Draft board card for [#{entry['issue']}](https://github.com/{REPO}/issues/{entry['issue']}), suggested by `{login}`.",
        "",
        f"**{entry['title']}** · {entry['category']} · proposed",
        f"> {entry['summary']}",
        "",
    ]
    if use:
        lines += [f"How they'd use it: {use}", ""]
    if hints:
        lines += ["**Might overlap with** (merge into one card's `includes` instead, if so)",
                  *[f"- {item['title']} (`{item['id']}`, shares: {', '.join(shared[:5])})" for _, item, shared in hints], ""]
    lines += ["Check the wording, the category and the duplicates, edit `docs/assets/feature-requests.json` on this branch if needed, then merge to put it on the board. Close to skip it. Nothing changes until this is merged."]
    return "\n".join(lines) + "\n"


def apply(root, issue):
    """Add the card and rebuild the board. Returns (title, body) for the pull request, or None."""
    items = board.load_catalogue(root)
    entry, why = draft(issue, items)
    if entry is None:
        print(f"Skipping #{issue['number']}: {why}.")
        return None
    path = root / CATALOGUE
    published = ("docs/features.html", "docs/index.html")
    before = {name: (root / name).read_text() for name in (CATALOGUE, *published)}
    try:
        path.write_text(admin.dump(items + [entry]))
        board.render(root)
    except Exception:
        for name, text in before.items():
            (root / name).write_text(text)
        raise
    hints = similar(entry["title"], entry["summary"], items)
    entry = {**entry, "issue": issue["number"]}
    return f"Feature board: {entry['title']}", pr_text(entry, issue, hints)


def pending():
    items = board.load_catalogue()
    linked = {n for item in items for n in item.get("issues", [])}
    found = api("issues?state=open&labels=enhancement&per_page=100")
    return [i["number"] for i in found if not i.get("pull_request") and i["number"] not in linked and draft(i, items)[0]]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--issue", type=int)
    parser.add_argument("--issue-file")
    parser.add_argument("--pending", action="store_true")
    parser.add_argument("--title-file")
    parser.add_argument("--body-file")
    args = parser.parse_args()
    if args.pending:
        print(" ".join(str(n) for n in pending()))
        return
    if args.issue_file:
        issue = json.loads(Path(args.issue_file).read_text())
    elif args.issue:
        issue = api(f"issues/{args.issue}")
    else:
        parser.error("give --issue, --issue-file or --pending")
    result = apply(ROOT, issue)
    if result and args.title_file and args.body_file:
        Path(args.title_file).write_text(result[0])
        Path(args.body_file).write_text(result[1])
    if result:
        print(result[1])


if __name__ == "__main__":
    main()
