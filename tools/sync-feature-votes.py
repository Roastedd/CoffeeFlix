#!/usr/bin/env python3
"""Publish vote totals for the feature board and keep the voting thread in step with the catalogue.

Each idea in docs/assets/feature-requests.json has one comment in the voting thread (the issue named in
docs/assets/feature-thread.json), found by a hidden marker. The 👍 reactions on those comments are the
votes. This script reads them into a small votes.json snapshot, so visitors don't each spend GitHub's
60-requests-an-hour anonymous API limit. With --sync-comments it also posts a comment for any idea that
has none and rewrites comments whose wording or status changed. It never deletes comments.
"""
import argparse
import importlib.util
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = "Roastedd/CoffeeFlix"
TRUSTED = {"roastedd", "github-actions[bot]"}  # only comments by these accounts can carry votes
MARKER = re.compile(r"<!--\s*coffeeflix-feature:\s*([a-z0-9-]+)\s*-->")
PAGE_LIMIT = 10  # 1,000 comments; more than that and the totals could be incomplete
MAX_NEW = 30  # a run never posts more than this many comments

_spec = importlib.util.spec_from_file_location("render_feature_board", ROOT / "tools/render-feature-board.py")
_render = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_render)
STATUSES = _render.STATUSES


def api(method, url, body=None):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "coffeeflix-feature-votes",
               "X-GitHub-Api-Version": "2022-11-28"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        hint = ""
        if error.code in (404, 410) and "/issues/" in url:
            hint = " The voting thread may have been deleted: open a new issue and put its number in docs/assets/feature-thread.json."
        raise SystemExit(f"GitHub returned {error.code} for {method} {url}: {error.read().decode(errors='replace')[:300]}{hint}")
    except (urllib.error.URLError, TimeoutError) as error:
        raise SystemExit(f"Could not reach GitHub for {method} {url}: {error}")


def load_catalogue():
    items = json.loads((ROOT / "docs/assets/feature-requests.json").read_text())
    issue = json.loads((ROOT / "docs/assets/feature-thread.json").read_text())["issue"]
    if type(issue) is not int or issue <= 0:
        raise SystemExit("docs/assets/feature-thread.json needs a positive issue number.")
    ids = [item["id"] for item in items]
    if len(set(ids)) != len(ids) or not all(re.fullmatch(r"[a-z0-9-]+", i) for i in ids):
        raise SystemExit("Feature ids must be unique and use only a-z, 0-9 and hyphens.")
    return items, issue


def fetch_comments(issue):
    comments = []
    for page in range(1, PAGE_LIMIT + 1):
        batch = api("GET", f"https://api.github.com/repos/{REPO}/issues/{issue}/comments?per_page=100&page={page}")
        comments.extend(batch)
        if len(batch) < 100:
            return comments
    raise SystemExit("The voting thread has too many comments to read in full; start a new thread.")


def index_comments(comments):
    """Map feature id -> its voting comment. Only trusted authors count, and the earliest comment wins."""
    found = {}
    for comment in comments:
        if (comment.get("user") or {}).get("login", "").lower() not in TRUSTED:
            continue
        marker = MARKER.search(comment.get("body") or "")
        if not marker:
            continue
        if marker.group(1) in found:
            print(f"warning: more than one voting comment for {marker.group(1)}; using the first", file=sys.stderr)
        else:
            found[marker.group(1)] = comment
    return found


def feature_body(item):
    source = " · ".join(([item["source"]] if item.get("source") else []) + [f"[GitHub #{n}](https://github.com/{REPO}/issues/{n})" for n in item.get("issues", [])])
    source = f"\n\n**Source:** {source}" if source else ""
    includes = ""
    if item.get("includes"):
        includes = "\n\n**Includes:**\n\n" + "\n".join(f"- {line}" for line in item["includes"])
    reply = f"\n\n**Reply from the maintainer:** {item['reply']}" if item.get("reply") else ""
    return f"""<!-- coffeeflix-feature: {item['id']} -->
<!-- coffeeflix-status: {item['status']} -->

### {item['title']}

{item['summary']}{includes}

**Category:** {item['category']}

**Status:** {STATUSES[item['status']]}{source}{reply}

**Vote:** add a 👍 reaction to this comment. You can support several ideas and withdraw a vote by clicking your 👍 again. Votes help set priorities; they are not a promise of implementation."""


def same_text(a, b):
    return a.replace("\r\n", "\n").strip() == b.replace("\r\n", "\n").strip()


def plan(items, indexed):
    """Return (ideas without a voting comment, [(comment, new body)] for comments that no longer match)."""
    create = [item for item in items if item["id"] not in indexed]
    edits = [(indexed[item["id"]], feature_body(item)) for item in items
             if item["id"] in indexed and not same_text(indexed[item["id"]]["body"], feature_body(item))]
    return create, edits


def snapshot(items, indexed, generated_at):
    features = {}
    for item in items:
        comment = indexed.get(item["id"])
        features[item["id"]] = ({"commentId": comment["id"], "votes": (comment.get("reactions") or {}).get("+1", 0)}
                                if comment else {"votes": None, "missing": True})
    return {"generatedAt": generated_at, "features": features}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", help="write the votes.json snapshot to this path")
    parser.add_argument("--sync-comments", action="store_true", help="post missing voting comments and fix changed ones")
    parser.add_argument("--dry-run", action="store_true", help="show what --sync-comments would do without doing it")
    args = parser.parse_args()
    if args.sync_comments and not args.dry_run and not os.environ.get("GITHUB_TOKEN"):
        raise SystemExit("--sync-comments needs GITHUB_TOKEN (or use --dry-run).")

    items, issue = load_catalogue()
    url = f"https://api.github.com/repos/{REPO}/issues/{issue}/comments"
    indexed = index_comments(fetch_comments(issue))
    if args.sync_comments:
        create, edits = plan(items, indexed)
        if len(create) > MAX_NEW:
            raise SystemExit(f"Refusing to post {len(create)} comments at once (limit {MAX_NEW}).")
        for item in create:
            print(f"{'Would post' if args.dry_run else 'Posting'} a voting comment for {item['id']}.")
            if not args.dry_run:
                indexed[item["id"]] = api("POST", url, {"body": feature_body(item)})
                time.sleep(1)  # GitHub asks for a pause between writes
        for comment, body in edits:
            print(f"{'Would update' if args.dry_run else 'Updating'} the voting comment {comment['id']}.")
            if not args.dry_run:
                api("PATCH", f"https://api.github.com/repos/{REPO}/issues/comments/{comment['id']}", {"body": body})
                time.sleep(1)
        print(f"{len(create)} comment(s) to post, {len(edits)} to update{' (dry run)' if args.dry_run else ''}.")
    if args.out:
        generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        Path(args.out).write_text(json.dumps(snapshot(items, indexed, generated_at), indent=1) + "\n")
        print(f"Wrote {len(items)} vote totals to {args.out}.")


if __name__ == "__main__":
    main()
