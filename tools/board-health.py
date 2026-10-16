#!/usr/bin/env python3
"""Check that the public board and roadmap match what has really shipped; print what needs a person.

  board-health.py [--offline]

Checks: the "Just shipped" version is the newest published release; every shipped idea names its version; the roadmap
was reviewed in the last 30 days; ideas In progress for over 60 days are listed (docs/assets/feature-requests.json
`since` dates, set when the status was last changed); and no shipped idea still has an open issue behind it.
Exit status 1 when there is something to look at; 2 when the check itself could not run. --offline skips the GitHub calls.
"""
import argparse
import json
import os
import re
import sys
import urllib.request
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = "Roastedd/CoffeeFlix"


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def check(items, roadmap, latest_page, published, open_issues, today):
    out = []
    if published and latest_page and vkey(latest_page) < vkey(published):
        out.append(f"'Just shipped' shows {latest_page} but {published} is published: run Roadmap after a release.")
    for i in items:
        if i["status"] == "shipped" and not i.get("shipped_in"):
            out.append(f"'{i['id']}' is shipped but has no shipped_in version.")
        if i["status"] == "shipped":
            for n in i.get("issues", []):
                if n in open_issues:
                    out.append(f"'{i['id']}' shipped but issue #{n} is open: run Tell people their idea shipped.")
        if i["status"] == "in-progress" and i.get("since"):
            days = (today - date.fromisoformat(i["since"])).days
            if days > 60:
                out.append(f"'{i['id']}' has been In progress for {days} days: ship it, move it back or say why.")
    age = (today - date.fromisoformat(roadmap["updated"])).days
    if age > 30:
        out.append(f"The roadmap was last reviewed {age} days ago.")
    return out


def get(url):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "coffeeflix-board"}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"  # a runner shares its address, so anonymous calls hit the limit
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    items = json.loads((ROOT / "docs/assets/feature-requests.json").read_text())
    roadmap = json.loads((ROOT / "docs/assets/roadmap.json").read_text())
    page = json.loads((ROOT / "docs/assets/latest-release.json").read_text())["version"]
    published, open_issues = None, set()
    if not args.offline:
        published = get(f"https://api.github.com/repos/{REPO}/releases/latest")["tag_name"].lstrip("v")
        wanted = {n for i in items if i["status"] == "shipped" for n in i.get("issues", [])}
        for n in wanted:
            if get(f"https://api.github.com/repos/{REPO}/issues/{n}")["state"] == "open":
                open_issues.add(n)
    problems = check(items, roadmap, page, published, open_issues, date.today())
    print("\n".join(problems) if problems else "The board is up to date.")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as error:  # a broken check is not a problem with the board
        print(f"The check could not run: {error}", file=sys.stderr)
        sys.exit(2)
