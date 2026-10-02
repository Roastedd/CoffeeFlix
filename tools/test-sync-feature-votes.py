#!/usr/bin/env python3
"""Offline checks for sync-feature-votes.py. GitHub is replaced by an in-memory thread; nothing is sent."""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sync", ROOT / "tools/sync-feature-votes.py")
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


class FakeGitHub:
    """Just enough of the issue-comments API: list (paged), create and edit."""

    def __init__(self, comments=None):
        self.comments = list(comments or [])
        self.calls = []

    def __call__(self, method, url, body=None):
        self.calls.append(method)
        if method == "GET":
            page = int(url.rsplit("page=", 1)[1])
            return self.comments[(page - 1) * 100:page * 100]
        if method == "POST":
            comment = make_comment(len(self.comments) + 1000, body["body"])
            self.comments.append(comment)
            return comment
        target = int(url.rsplit("/", 1)[1])
        comment = next(c for c in self.comments if c["id"] == target)
        comment["body"] = body["body"]
        return comment


def make_comment(comment_id, body, author="github-actions[bot]", votes=0):
    return {"id": comment_id, "user": {"login": author}, "body": body, "reactions": {"+1": votes, "heart": 99}}


def run_main(github, *args):
    out = io.StringIO()
    with mock.patch.object(sync, "api", github), mock.patch.object(sync.time, "sleep"), \
            mock.patch.dict(os.environ, {"GITHUB_TOKEN": "test"}), mock.patch.object(sys, "argv", ["sync", *args]), \
            contextlib.redirect_stdout(out):
        sync.main()
    return out.getvalue()


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.items, self.issue = sync.load_catalogue()

    def thread(self, **overrides):
        return {item["id"]: make_comment(1000 + i, sync.feature_body(item), **overrides) for i, item in enumerate(self.items)}

    def test_catalogue_is_consistent(self):
        self.assertGreater(len(self.items), 1)
        self.assertGreater(self.issue, 0)
        self.assertTrue(all("commentId" not in item for item in self.items))

    def test_matching_thread_needs_nothing(self):
        create, edits = sync.plan(self.items, self.thread())
        self.assertEqual((create, edits), ([], []))

    def test_line_endings_and_trailing_space_are_not_edits(self):
        thread = self.thread()
        first = thread[self.items[0]["id"]]
        first["body"] = first["body"].replace("\n", "\r\n") + "\r\n"
        self.assertEqual(sync.plan(self.items, thread), ([], []))

    def test_changed_status_updates_only_that_comment(self):
        thread = self.thread()
        target = self.items[1]
        thread[target["id"]]["body"] = sync.feature_body({**target, "status": "shipped"})
        create, edits = sync.plan(self.items, thread)
        self.assertEqual(create, [])
        self.assertEqual([c["id"] for c, _ in edits], [thread[target["id"]]["id"]])
        self.assertIn(f"**Status:** {sync.STATUSES[target['status']]}", edits[0][1])
        self.assertIn(f"coffeeflix-status: {target['status']}", edits[0][1])

    def test_ideas_without_a_comment_are_posted(self):
        thread = self.thread()
        gone = self.items[2]["id"]
        del thread[gone]
        create, edits = sync.plan(self.items, thread)
        self.assertEqual([item["id"] for item in create], [gone])
        self.assertEqual(edits, [])

    def test_only_trusted_authors_carry_votes(self):
        real = make_comment(1, sync.feature_body(self.items[0]), votes=2)
        fake = make_comment(2, f"<!-- coffeeflix-feature: {self.items[0]['id']} -->", author="mallory", votes=500)
        indexed = sync.index_comments([fake, real])
        self.assertEqual(indexed[self.items[0]["id"]]["id"], 1)
        self.assertEqual(sync.index_comments([fake]), {})
        owner = make_comment(3, sync.feature_body(self.items[1]), author="Roastedd")
        self.assertIn(self.items[1]["id"], sync.index_comments([owner]))

    def test_duplicate_markers_use_the_earliest_comment(self):
        first = make_comment(1, sync.feature_body(self.items[0]), votes=3)
        second = make_comment(2, sync.feature_body(self.items[0]), votes=40)
        with contextlib.redirect_stderr(io.StringIO()):
            indexed = sync.index_comments([first, second])
        self.assertEqual(indexed[self.items[0]["id"]]["id"], 1)

    def test_comments_without_a_marker_are_ignored(self):
        self.assertEqual(sync.index_comments([make_comment(1, "just chatting", votes=9)]), {})

    def test_snapshot_counts_only_thumbs_up_on_registered_comments(self):
        thread = self.thread()
        a, b = self.items[0], self.items[1]
        thread[a["id"]]["reactions"]["+1"] = 4
        data = sync.snapshot(self.items, thread, "2026-10-01T00:00:00Z")
        self.assertEqual(data["generatedAt"], "2026-10-01T00:00:00Z")
        self.assertEqual(data["features"][a["id"]], {"commentId": thread[a["id"]]["id"], "votes": 4})
        self.assertEqual(data["features"][b["id"]]["votes"], 0)
        self.assertEqual(set(data["features"]), {item["id"] for item in self.items})

    def test_snapshot_never_turns_a_missing_comment_into_zero(self):
        thread = self.thread()
        gone = self.items[0]["id"]
        del thread[gone]
        self.assertEqual(sync.snapshot(self.items, thread, "x")["features"][gone], {"votes": None, "missing": True})

    def test_fetch_follows_pages_and_refuses_an_unbounded_thread(self):
        with mock.patch.object(sync, "api", FakeGitHub([{"id": i} for i in range(101)])):
            self.assertEqual(len(sync.fetch_comments(self.issue)), 101)
        with mock.patch.object(sync, "api", lambda method, url, body=None: [{"id": 1}] * 100):
            with self.assertRaises(SystemExit):
                sync.fetch_comments(self.issue)

    def test_empty_thread_is_filled_once_and_totals_are_published(self):
        github = FakeGitHub()
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder, "votes.json")
            run_main(github, "--sync-comments", "--out", str(out))
            self.assertEqual(github.calls.count("POST"), len(self.items))
            published = json.loads(out.read_text())["features"]
            self.assertEqual(set(published), {item["id"] for item in self.items})
            self.assertTrue(all(isinstance(entry["commentId"], int) and entry["votes"] == 0 for entry in published.values()))
            github.calls.clear()
            run_main(github, "--sync-comments", "--out", str(out))  # a second run finds them all
            self.assertEqual([c for c in github.calls if c != "GET"], [])

    def test_a_status_change_reaches_github_and_keeps_the_comment(self):
        github = FakeGitHub()
        run_main(github, "--sync-comments")
        original = {c["id"] for c in github.comments}
        target = self.items[0]
        with mock.patch.object(sync, "load_catalogue", lambda: ([{**target, "status": "shipped"}, *self.items[1:]], self.issue)):
            run_main(github, "--sync-comments")
        self.assertEqual({c["id"] for c in github.comments}, original)
        self.assertIn("**Status:** Shipped", github.comments[0]["body"])
        self.assertEqual(github.calls.count("PATCH"), 1)

    def test_dry_run_changes_nothing(self):
        github = FakeGitHub()
        said = run_main(github, "--sync-comments", "--dry-run")
        self.assertEqual(github.calls, ["GET"])
        self.assertIn("Would post", said)

    def test_a_runaway_run_is_refused(self):
        many = [{**self.items[0], "id": f"idea-{i}"} for i in range(sync.MAX_NEW + 1)]
        github = FakeGitHub()
        with mock.patch.object(sync, "load_catalogue", lambda: (many, self.issue)):
            with self.assertRaises(SystemExit):
                run_main(github, "--sync-comments")
        self.assertEqual(github.calls, ["GET"])

    def test_writing_needs_a_token(self):
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(sys, "argv", ["sync", "--sync-comments"]):
            with self.assertRaises(SystemExit):
                sync.main()


if __name__ == "__main__":
    unittest.main()
