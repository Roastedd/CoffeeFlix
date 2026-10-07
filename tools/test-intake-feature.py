#!/usr/bin/env python3
"""intake-feature.py drafts one safe, proposed card per new feature-idea issue and skips everything else."""
import importlib.util
import json
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("intake", ROOT / "tools/intake-feature.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def body(idea, use="", area="Something else"):
    return f"### What would you like CoffeeFlix to do?\n\n{idea}\n\n### How would you use it?\n\n{use or '_No response_'}\n\n### Which part of the app?\n\n{area}\n\n### Before you submit\n\n- [X] I checked."


def issue(number=50, title="Feature idea: Night mode", text="Dim the whole app at night.", **extra):
    return {"number": number, "state": "open", "title": title, "user": {"login": "someone"}, "labels": [{"name": "enhancement"}], "body": body(text), **extra}


assert mod.fields(body("A", "B", "YouTube")) == {"What would you like CoffeeFlix to do?": "A", "How would you use it?": "B", "Which part of the app?": "YouTube", "Before you submit": "- [X] I checked."}
assert mod.fields(body("A"))["How would you use it?"] == ""            # "_No response_" means empty
nasty = mod.plain("Hi @everyone [click](http://evil.example/x) <b>now</b> `code`\n\nthanks", 100)
assert "@" not in nasty and "http" not in nasty and "<" not in nasty and "\n" not in nasty, nasty
assert len(mod.plain("word " * 100, 40)) <= 41 and mod.plain("word " * 100, 40).endswith("…")

with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    for sub in ("docs/assets", "tools"):
        (root / sub).mkdir(parents=True)
    for name in ("docs/index.html", "docs/features.html", "docs/assets/feature-requests.json", "docs/assets/feature-thread.json", "docs/assets/roadmap.json"):
        shutil.copy(ROOT / name, root / name)
    for name in ("render-feature-board.py", "render-roadmap.py"):
        shutil.copy(ROOT / "tools" / name, root / "tools" / name)
    catalogue = lambda: json.loads((root / "docs/assets/feature-requests.json").read_text())
    before = len(catalogue())

    title, text = mod.apply(root, issue())
    added = catalogue()[-1]
    assert len(catalogue()) == before + 1 and title == "Feature board: Night mode"
    assert added["status"] == "proposed" and added["issues"] == [50] and added["id"] == "night-mode" and added["category"] == "Other"
    assert "Night mode" in (root / "docs/features.html").read_text()
    assert "proposed" in text and "`someone`" in text and "@" not in text

    snapshot = (root / "docs/assets/feature-requests.json").read_text()
    for skipped in (issue(50),                                             # already on the board
                    issue(51, state="closed"), issue(52, pull_request={}),
                    {**issue(53), "labels": []}, {**issue(54), "body": "A bug report, no form."},
                    {**issue(55), "body": body("")}):
        assert mod.apply(root, skipped) is None, skipped["number"]
    assert (root / "docs/assets/feature-requests.json").read_text() == snapshot

    # similar ideas are hinted, and the id never collides with an existing one
    mod.apply(root, issue(60, "Feature idea: Night mode", "Dim the app at night, again."))
    assert catalogue()[-1]["id"] == "night-mode-2"
    again = issue(61, "Feature idea: YouTube dislike counts", "Show the Return YouTube Dislike counts on videos.")
    assert "youtube-community" in mod.apply(root, again)[1]
print("intake-feature ok")
