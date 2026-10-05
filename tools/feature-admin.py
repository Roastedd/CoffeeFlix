#!/usr/bin/env python3
"""Edit the feature board in your browser: statuses, replies, wording, adding and removing ideas.

Run `python3 tools/feature-admin.py`. It serves a page on this computer only, saves to
docs/assets/feature-requests.json, rebuilds the board and homepage roadmap, and can publish them.
"""
import argparse
import contextlib
import io
import http.server
import importlib.util
import json
import re
import secrets
import subprocess
import sys
import threading
import webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("render_feature_board", HERE / "render-feature-board.py")
board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board)

CATALOGUE = "docs/assets/feature-requests.json"
PUBLISHED = (CATALOGUE, "docs/features.html", "docs/index.html")
KEYS = ("id", "category", "title", "summary", "includes", "source", "issues", "status", "shipped_in", "reply")


def slug(text, taken):
    base = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:40].strip("-") or "idea"
    candidate, n = base, 2
    while candidate in taken:
        candidate, n = f"{base}-{n}", n + 1
    return candidate


def clean(ideas):
    """Turn what the page sends into catalogue entries; raises ValueError with a readable message."""
    if not isinstance(ideas, list):
        raise ValueError("Expected a list of ideas.")
    taken, out = set(), []
    for raw in ideas:
        if not isinstance(raw, dict):
            raise ValueError("Expected a list of ideas.")
        text = lambda key: str(raw.get(key) or "").strip()
        title = text("title")
        if not title:
            raise ValueError("Every idea needs a title.")
        for key in ("category", "summary"):
            if not text(key):
                raise ValueError(f"“{title}” needs a {key}.")
        idea_id = text("id") or slug(title, taken)
        issues = []
        for part in re.split(r"[,\s]+", str(raw.get("issues") or "")):
            if part:
                number = part.lstrip("#")
                if not number.isdigit() or int(number) <= 0:
                    raise ValueError(f"“{title}”: GitHub issue numbers must be numbers like 11, 12 (got “{part}”).")
                issues.append(int(number))
        includes = raw.get("includes") or []
        if isinstance(includes, str):
            includes = includes.splitlines()
        entry = {"id": idea_id, "category": text("category"), "title": title, "summary": text("summary"),
                 "includes": [line.strip() for line in includes if str(line).strip()],
                 "source": text("source"), "issues": issues, "status": text("status"), "reply": text("reply"),
                 "shipped_in": text("shipped_in").lstrip("vV")}
        for key in ("source", "issues", "reply", "shipped_in"):
            if not entry[key]:
                del entry[key]
        taken.add(idea_id)
        out.append(entry)
    try:
        board.validate_catalogue(out)
    except AssertionError as error:
        raise ValueError(str(error)) from None
    return out


def dump(items):
    """One idea per block, short number lists on one line, so git diffs stay readable."""
    blocks = []
    for item in items:
        lines = []
        for key in KEYS:
            if key not in item:
                continue
            value = item[key]
            if isinstance(value, list) and all(type(v) is int for v in value):
                text = json.dumps(value)
            else:
                text = json.dumps(value, ensure_ascii=False, indent=2).replace("\n", "\n    ")
            lines.append(f"    {json.dumps(key)}: {text}")
        blocks.append("  {\n" + ",\n".join(lines) + "\n  }")
    return "[\n" + ",\n".join(blocks) + "\n]\n"


def save(root, ideas):
    """Validate, write the catalogue and rebuild the page; put the old catalogue back if that fails."""
    items = clean(ideas)
    path = root / CATALOGUE
    before = {name: (root / name).read_text() for name in PUBLISHED}
    path.write_text(dump(items))
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            board.render(root)
    except Exception as error:
        for name, text in before.items():
            (root / name).write_text(text)
        raise ValueError(f"Couldn't rebuild the page: {error}") from None
    return items


def git(root, *args):
    done = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, timeout=90)
    return done.returncode, (done.stdout + done.stderr).strip()


def pending(root):
    code, out = git(root, "status", "--porcelain", "--", *PUBLISHED)
    return code == 0 and bool(out)


def publish(root):
    code, branch = git(root, "rev-parse", "--abbrev-ref", "HEAD")
    if code != 0:
        return False, "This folder isn't a git repository."
    if branch != "master":
        return False, f"You're on the “{branch}” branch. Switch to master to publish the board."
    if not pending(root):
        return False, "Nothing to publish: the board already matches the website."
    code, out = git(root, "commit", "-m", "Update the feature board", "--", *PUBLISHED)
    if code != 0:
        return False, f"Couldn't save the change in git:\n{out}"
    code, out = git(root, "push", "origin", "master")
    if code != 0:
        return False, f"Saved in git but couldn't push it to GitHub:\n{out}\nRun `git push` once you're online."
    return True, "Published. The website updates in a minute or two."


def state(root):
    thread = json.loads((root / "docs/assets/feature-thread.json").read_text())["issue"]
    return {"ideas": json.loads((root / CATALOGUE).read_text()),
            "statuses": [{"value": key, "label": label} for key, label in board.STATUSES.items()],
            "thread": f"{board.REPO_URL}/issues/{thread}", "pending": pending(root)}


class Handler(http.server.BaseHTTPRequestHandler):
    """Serves the editor page and its JSON API; every request needs the token and a local Host header."""

    def log_message(self, *args):
        pass

    def reply(self, code, body, kind="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", kind + "; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def authorised(self):
        port = self.server.server_address[1]
        given = self.headers.get("X-Token") or self.path.partition("?t=")[2]
        return self.headers.get("Host") in (f"127.0.0.1:{port}", f"localhost:{port}") and secrets.compare_digest(given, self.server.token)

    def do_GET(self):
        if not self.authorised():
            return self.reply(403, {"error": "Open the link printed in the terminal."})
        route = self.path.partition("?")[0]
        if route == "/":
            return self.reply(200, (HERE / "feature-admin.html").read_bytes(), "text/html")
        if route == "/api/state":
            return self.reply(200, state(self.server.root))
        self.reply(404, {"error": "Not found"})

    def do_POST(self):
        if not self.authorised() or self.headers.get("Content-Type", "").split(";")[0] != "application/json":
            return self.reply(403, {"error": "Not allowed."})
        root = self.server.root
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
            if self.path == "/api/save":
                save(root, body.get("ideas"))
                return self.reply(200, {"ok": True, **state(root)})
            if self.path == "/api/publish":
                save(root, body.get("ideas"))
                ok, message = publish(root)
                return self.reply(200, {"ok": ok, "message": message, **state(root)})
        except (ValueError, json.JSONDecodeError) as error:
            return self.reply(400, {"error": str(error)})
        self.reply(404, {"error": "Not found"})


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=board.ROOT, help="repository folder (default: this one)")
    parser.add_argument("--port", type=int, default=0, help="port to listen on (default: any free one)")
    parser.add_argument("--no-open", action="store_true", help="don't open the browser")
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.root, server.token = args.root.resolve(), secrets.token_urlsafe(16)
    url = f"http://127.0.0.1:{server.server_address[1]}/?t={server.token}"
    print(f"Feature board editor: {url}\nPress Ctrl+C to stop.", flush=True)
    if not args.no_open:
        threading.Timer(0.4, webbrowser.open, [url]).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    sys.exit(main())
