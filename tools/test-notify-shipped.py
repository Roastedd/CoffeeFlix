#!/usr/bin/env python3
"""notify-shipped.py decides what to say and what to close, without touching GitHub."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location("ns", Path(__file__).resolve().parents[1] / "tools/notify-shipped.py")
ns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ns)

card = lambda i, status, issues, **k: {"id": i, "title": i.title(), "status": status, "issues": issues, **k}
items = [card("a", "shipped", [1], shipped_in="2.5.0"), card("b", "shipped", [2], shipped_in="2.5.0"), card("c", "proposed", [2]),
         card("d", "shipped", [3], shipped_in="2.5.0"), card("e", "shipped", [], shipped_in="2.5.0")]
state = {1: "open", 2: "open", 3: "closed"}
acts = ns.plan(items, state, {})
kinds = [(a[0], a[1]) for a in acts]
assert ("comment", 1) in kinds and ("close", 1) in kinds          # shipped, its only card: tell and close
assert ("comment", 2) in kinds and ("close", 2) not in kinds      # a bundle, one card still to come: tell, keep open
assert ("comment", 3) in kinds and ("close", 3) not in kinds      # already closed: told once, never reopened/closed again
# a second run, with the comments there, does nothing
said = {n: [a[2] for a in acts if a[0] == "comment" and a[1] == n] for n in (1, 2, 3)}
again = ns.plan(items, {1: "closed", 2: "open", 3: "closed"}, said)
assert again == [], again
# an issue that is really a pull request (not in state) is skipped
assert ns.plan(items, {}, {}) == []
assert "shipped in [CoffeeFlix 2.5.0]" in acts[0][2] and "coffeeflix-shipped: a@2.5.0" in acts[0][2]
print("notify-shipped ok")
