#!/usr/bin/env python3
"""Tell the people who asked for an idea that it shipped, and close the issue when everything in it has.

  notify-shipped.py [--dry-run]

For every idea on the board with status "shipped", each GitHub issue listed in its `issues` gets one comment
saying which version brought it (found again by a hidden marker, so a re-run never repeats itself). An issue
is closed once every card that lists it has shipped; an issue that lists several ideas (a request with a
bundle of wishes) stays open until the last one is out. Issues already closed, and issues that are pull
requests, are left alone. Needs GITHUB_TOKEN with issues: write; --dry-run only reads and prints.
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = "Roastedd/CoffeeFlix"
SITE = "https://roastedd.github.io/CoffeeFlix/features.html"


def api(method, url, body=None):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "coffeeflix-board", "X-GitHub-Api-Version": "2022-11-28"}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = json.dumps(body).encode() if body is not None else None
    if data:
        headers["Content-Type"] = "application/json"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers, method=method), timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raise SystemExit(f"GitHub returned {error.code} for {method} {url}: {error.read().decode(errors='replace')[:300]}")


def marker(ident, version):
    return f"<!-- coffeeflix-shipped: {ident}@{version} -->"


def message(item):
    version = item["shipped_in"]
    return (f"{marker(item['id'], version)}\n"
            f"Good news: **{item['title']}** shipped in [CoffeeFlix {version}](https://github.com/{REPO}/releases/tag/v{version}). "
            f"Thank you for asking for it. If something you wanted is still missing, tell me here or on the "
            f"[feature board]({SITE}).")


def plan(items, issues_state, comments_by_issue):
    """What to do: [("comment", issue, text)] and [("close", issue)]. Pure, so it can be tested."""
    shipped = [i for i in items if i["status"] == "shipped" and i.get("shipped_in")]
    actions, by_issue = [], {}
    for item in items:
        for n in item.get("issues", []):
            by_issue.setdefault(n, []).append(item)
    for item in shipped:
        for n in item.get("issues", []):
            if issues_state.get(n) is None:  # not an issue we can see (or a pull request)
                continue
            if not any(marker(item["id"], item["shipped_in"]) in c for c in comments_by_issue.get(n, [])):
                actions.append(("comment", n, message(item)))
    for n, cards in sorted(by_issue.items()):
        if issues_state.get(n) == "open" and all(c["status"] == "shipped" for c in cards):
            actions.append(("close", n))
    return actions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    items = json.loads((ROOT / "docs/assets/feature-requests.json").read_text())
    wanted = sorted({n for i in items if i["status"] == "shipped" for n in i.get("issues", [])})
    state, comments = {}, {}
    for n in wanted:
        issue = api("GET", f"https://api.github.com/repos/{REPO}/issues/{n}")
        if "pull_request" in issue:
            continue
        state[n] = issue["state"]
        comments[n] = [c["body"] for c in api("GET", f"https://api.github.com/repos/{REPO}/issues/{n}/comments?per_page=100")]
    # an issue listed by cards that haven't shipped is still needed for the close decision
    for n in sorted({n for i in items for n in i.get("issues", [])} - set(wanted)):
        issue = api("GET", f"https://api.github.com/repos/{REPO}/issues/{n}")
        if "pull_request" not in issue:
            state[n] = issue["state"]
    actions = plan(items, state, comments)
    if not actions:
        print("Nothing to say or close.")
    for action in actions:
        print(("would " if args.dry_run else "") + f"{action[0]} on #{action[1]}")
        if args.dry_run:
            continue
        if action[0] == "comment":
            api("POST", f"https://api.github.com/repos/{REPO}/issues/{action[1]}/comments", {"body": action[2]})
        else:
            api("PATCH", f"https://api.github.com/repos/{REPO}/issues/{action[1]}", {"state": "closed", "state_reason": "completed"})


if __name__ == "__main__":
    main()
