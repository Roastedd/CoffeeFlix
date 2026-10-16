#!/usr/bin/env python3
"""update-just-shipped.py writes the board's "Just shipped" block from release notes, without fetching anything."""
import importlib.util
import json
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("js", ROOT / "tools/update-just-shipped.py")
js = importlib.util.module_from_spec(spec)
spec.loader.exec_module(js)

BODY = """Your GamePad finally earns its keep: browse on both screens. More words here.

- Choose **where** sound plays
- Pick a [start screen](https://example.com/x) (Settings > Start screen)
* Better `suggestions`
- Four
- Five
- Six is one too many

☕ Enjoying CoffeeFlix? You can support it on [Ko-fi](https://ko-fi.com/x).
"""

with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    for rel in ["docs/features.html", "docs/index.html", "docs/assets/feature-thread.json", "docs/assets/roadmap.json",
                "tools/render-feature-board.py", "tools/render-roadmap.py"]:
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / rel, root / rel)
    catalogue = json.loads((ROOT / "docs/assets/feature-requests.json").read_text())
    catalogue.append({"id": "zz-test", "category": "Controls", "title": "Test idea <b>", "summary": "Small.", "status": "shipped", "shipped_in": "9.9.9"})
    (root / "docs/assets/feature-requests.json").write_text(json.dumps(catalogue))
    (root / "release").mkdir()
    (root / "release/features.json").write_text(json.dumps({"9.9.9": ["startup-screen"]}))

    r = js.update(root, "9.9.9", BODY)
    assert r["highlights"][0] == "Your GamePad finally earns its keep: browse on both screens.", r["highlights"]
    assert r["highlights"][1:] == ["Choose where sound plays", "Pick a start screen (Settings > Start screen)", "Better suggestions", "Four"], r
    assert r["note"].startswith("From this board: Pick your start screen and Test idea <b>."), r["note"]
    page = (root / "docs/features.html").read_text()
    assert "CoffeeFlix 9.9.9 is out" in page and "releases/tag/v9.9.9" in page
    assert "Test idea &lt;b&gt;" in page and "Settings &gt; Start screen" in page   # escaped
    assert "Ko-fi" not in page.split("<!-- BEGIN JUST SHIPPED -->")[1].split("<!-- END JUST SHIPPED -->")[0]
    assert page.count("<!-- BEGIN JUST SHIPPED -->") == 1

    # a "Board:" line is the page's first line and is not repeated
    r = js.update(root, "9.9.10", "Long opening sentence of the notes.\n\nBoard: Photos open fast and look right.\n\n- One\n- Two\n")
    assert r["highlights"] == ["Photos open fast and look right.", "One", "Two"], r["highlights"]
    # an older version never replaces a newer one
    try:
        js.update(root, "9.9.7", BODY)
        raise AssertionError("went back to an older version")
    except SystemExit:
        pass
    # notes without a list: the opening paragraph; no board ideas: a plain thanks
    r = js.update(root, "10.0.0", "Just one paragraph of notes, nothing else.")
    assert r["highlights"] == ["Just one paragraph of notes, nothing else."] and r["note"] == "Thanks for voting, and keep the ideas coming."
    # the same version again is fine (a re-run of the workflow)
    js.update(root, "10.0.0", "Just one paragraph of notes, nothing else.")
print("just shipped tests passed")
