#!/usr/bin/env python3
"""Checks for tools/feature-admin.py against throwaway copies of the repository."""
import http.client
import importlib.util
import json
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = importlib.util.spec_from_file_location("feature_admin", HERE / "feature-admin.py")
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)


def run(cwd, *args):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


class Workspace(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo, remote = self.tmp / "repo", self.tmp / "remote.git"
        (self.repo / "docs/assets").mkdir(parents=True)
        for name in ("features.html", "assets/feature-requests.json", "assets/feature-thread.json"):
            shutil.copy(ROOT / "docs" / name, self.repo / "docs" / name)
        (self.repo / "other.txt").write_text("unrelated\n")
        run(self.tmp, "git", "init", "-q", "--bare", "-b", "master", str(remote))
        run(self.repo, "git", "init", "-q", "-b", "master")
        run(self.repo, "git", "config", "user.name", "Tester")
        run(self.repo, "git", "config", "user.email", "tester@example.com")
        run(self.repo, "git", "remote", "add", "origin", str(remote))
        run(self.repo, "git", "add", ".")
        run(self.repo, "git", "commit", "-q", "-m", "start")
        run(self.repo, "git", "push", "-q", "origin", "master")
        self.ideas = json.loads((self.repo / admin.CATALOGUE).read_text())

    def editable(self):
        return [{**i, "includes": "\n".join(i.get("includes", [])), "issues": ", ".join(map(str, i.get("issues", [])))}
                for i in self.ideas]


class Cleaning(unittest.TestCase):
    base = {"category": "Controls", "title": "Brand new idea!", "summary": "Do a thing.", "status": "proposed"}

    def test_new_idea_gets_a_readable_unique_id(self):
        out = admin.clean([self.base, {**self.base}])
        self.assertEqual([i["id"] for i in out], ["brand-new-idea", "brand-new-idea-2"])

    def test_empty_optional_fields_are_dropped_and_issue_numbers_parsed(self):
        out = admin.clean([{**self.base, "issues": "#11, 12", "reply": "  ", "includes": "a\n\n b ", "source": ""}])[0]
        self.assertEqual(out["issues"], [11, 12])
        self.assertEqual(out["includes"], ["a", "b"])
        self.assertNotIn("reply", out)
        self.assertNotIn("source", out)

    def test_bad_input_is_explained(self):
        for bad, text in [({**self.base, "title": " "}, "title"), ({**self.base, "issues": "abc"}, "numbers"),
                          ({**self.base, "status": "winning"}, "status"), ({**self.base, "summary": ""}, "summary"),
                          ({**self.base, "category": ""}, "category")]:
            with self.assertRaises(ValueError, msg=text) as caught:
                admin.clean([bad])
            self.assertIn(text, str(caught.exception).lower())

    def test_shipped_in_is_kept_cleaned_and_only_for_shipped_ideas(self):
        out = admin.clean([{**self.base, "status": "shipped", "shipped_in": " v2.4.0 "}])[0]
        self.assertEqual(out["shipped_in"], "2.4.0")
        self.assertNotIn("shipped_in", admin.clean([{**self.base, "status": "shipped", "shipped_in": ""}])[0])
        for bad in ({"status": "planned", "shipped_in": "2.4.0"}, {"status": "shipped", "shipped_in": "soon"}):
            with self.assertRaises(ValueError):
                admin.clean([{**self.base, **bad}])

    def test_existing_ids_are_kept(self):
        self.assertEqual(admin.clean([{**self.base, "id": "keep-me"}])[0]["id"], "keep-me")

    def test_dump_round_trips_and_keeps_issue_lists_short(self):
        items = json.loads((ROOT / admin.CATALOGUE).read_text())
        text = admin.dump(items)
        self.assertEqual(json.loads(text), items)
        self.assertIn('"issues": [11, 12]', text)


class Saving(Workspace):
    def test_save_rewrites_catalogue_and_page(self):
        ideas = self.editable()
        ideas[0]["reply"] = "Thanks <b>all</b>"
        ideas[0]["status"] = "shipped"
        del ideas[1]
        admin.save(self.repo, ideas)
        saved = json.loads((self.repo / admin.CATALOGUE).read_text())
        self.assertEqual(len(saved), len(self.ideas) - 1)
        self.assertEqual(saved[0]["reply"], "Thanks <b>all</b>")
        page = (self.repo / "docs/features.html").read_text()
        self.assertIn("Thanks &lt;b&gt;all&lt;/b&gt;", page)
        self.assertNotIn(f'id="{self.ideas[1]["id"]}"', page)
        self.assertIn(f'<span class="badge" data-state="shipped">Shipped</span>', page)

    def test_invalid_save_changes_nothing(self):
        before = (self.repo / admin.CATALOGUE).read_text()
        ideas = self.editable()
        ideas[0]["title"] = ""
        with self.assertRaises(ValueError):
            admin.save(self.repo, ideas)
        self.assertEqual((self.repo / admin.CATALOGUE).read_text(), before)

    def test_failed_rebuild_restores_the_catalogue(self):
        before = (self.repo / admin.CATALOGUE).read_text()
        (self.repo / "docs/features.html").write_text("<html>no markers</html>")
        with self.assertRaises(ValueError):
            admin.save(self.repo, self.editable())
        self.assertEqual((self.repo / admin.CATALOGUE).read_text(), before)


class Publishing(Workspace):
    def test_publish_commits_only_the_board_and_pushes(self):
        (self.repo / "other.txt").write_text("my other work\n")
        ideas = self.editable()
        ideas[0]["reply"] = "Hello"
        admin.save(self.repo, ideas)
        self.assertTrue(admin.pending(self.repo))
        ok, message = admin.publish(self.repo)
        self.assertTrue(ok, message)
        self.assertEqual(sorted(run(self.repo, "git", "show", "--name-only", "--format=", "HEAD").splitlines()),
                         sorted(admin.PUBLISHED))
        self.assertEqual(run(self.repo, "git", "status", "--porcelain"), "M other.txt")
        self.assertEqual(run(self.repo, "git", "rev-parse", "HEAD"), run(self.repo, "git", "rev-parse", "origin/master"))
        self.assertEqual(run(self.repo, "git", "log", "-1", "--format=%B").strip(), "Update the feature board")

    def test_nothing_to_publish(self):
        ok, message = admin.publish(self.repo)
        self.assertFalse(ok)
        self.assertIn("Nothing to publish", message)

    def test_other_branches_are_refused(self):
        run(self.repo, "git", "checkout", "-q", "-b", "side")
        admin.save(self.repo, self.editable()[1:])
        ok, message = admin.publish(self.repo)
        self.assertFalse(ok)
        self.assertIn("side", message)
        self.assertEqual(run(self.repo, "git", "log", "--oneline").count("\n"), 0)


class Server(Workspace):
    def setUp(self):
        super().setUp()
        self.server = admin.http.server.ThreadingHTTPServer(("127.0.0.1", 0), admin.Handler)
        self.server.root, self.server.token = self.repo, "secret"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.port = self.server.server_address[1]

    def request(self, method, path, body=None, headers=None, host=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port)
        conn.putrequest(method, path, skip_host=True)
        conn.putheader("Host", host or f"127.0.0.1:{self.port}")
        for key, value in (headers or {}).items():
            conn.putheader(key, value)
        data = json.dumps(body).encode() if body is not None else None
        if data is not None:
            conn.putheader("Content-Length", str(len(data)))
        conn.endheaders(data)
        res = conn.getresponse()
        try:
            return res.status, res.read()
        finally:
            conn.close()

    def test_token_and_host_are_required(self):
        self.assertEqual(self.request("GET", "/")[0], 403)
        self.assertEqual(self.request("GET", "/?t=wrong")[0], 403)
        self.assertEqual(self.request("GET", "/?t=secret", host="evil.example")[0], 403)
        status, body = self.request("GET", "/?t=secret")
        self.assertEqual(status, 200)
        self.assertIn(b"Feature board editor", body)

    def test_state_and_save_over_http(self):
        status, body = self.request("GET", "/api/state", headers={"X-Token": "secret"})
        state = json.loads(body)
        self.assertEqual(status, 200)
        self.assertEqual(len(state["ideas"]), len(self.ideas))
        self.assertIn({"value": "planned", "label": "Planned"}, state["statuses"])
        ideas = self.editable()
        ideas[2]["reply"] = "Over HTTP"
        headers = {"X-Token": "secret", "Content-Type": "application/json"}
        status, body = self.request("POST", "/api/save", {"ideas": ideas}, headers)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads((self.repo / admin.CATALOGUE).read_text())[2]["reply"], "Over HTTP")
        status, body = self.request("POST", "/api/save", {"ideas": [{"title": ""}]}, headers)
        self.assertEqual(status, 400)
        self.assertIn("title", json.loads(body)["error"].lower())

    def test_posts_must_be_json_with_the_token(self):
        self.assertEqual(self.request("POST", "/api/save", {"ideas": []}, {"X-Token": "secret"})[0], 403)
        self.assertEqual(self.request("POST", "/api/save", {"ideas": []}, {"Content-Type": "application/json"})[0], 403)


if __name__ == "__main__":
    unittest.main()
