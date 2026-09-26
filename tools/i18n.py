#!/usr/bin/env python3
"""Translations of the interface: the English text in tr("...") and N_("...") across src/, and
each language's text for it in content/lang/<code>.json.

  tools/i18n.py              # what each language is missing, and mistakes (the default)
  tools/i18n.py update       # rewrites the files in the code's order: new text gets "", and text
                             # the code no longer has is dropped
  tools/i18n.py context      # the English text with where it's used (for translating)

An empty translation shows the English. A translation has to keep the English text's % codes
(%s, %d, %zu...) in the same order, or the app shows the English instead. Names in braces
({weekday}) have to stay too, in any order.
"""

import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "content", "lang")
CALL = re.compile(r'\b(?:tr|N_)\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\)')
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def decode(body):
    """A C string literal's text, as the compiler reads it (\\x takes every hex digit after it)."""
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        n = body[i + 1]
        i += 2
        if n == "x":
            j = i
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i:j], 16) & 0xFF)
            i = j
        elif n in "01234567":
            start = j = i - 1  # up to three octal digits, from n
            while j < len(body) and j < start + 3 and body[j] in "01234567":
                j += 1
            out.append(int(body[start:j], 8) & 0xFF)
            i = j
        else:
            out += {"n": b"\n", "t": b"\t", "r": b"\r", '"': b'"', "'": b"'", "\\": b"\\", "?": b"?"}[n]
    return out.decode("utf-8")


def extract():
    """{english: [file:line, ...]} in the order the code has them."""
    found = collections.OrderedDict()
    files = sorted(glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True) +
                   glob.glob(os.path.join(ROOT, "src", "**", "*.hpp"), recursive=True))
    for path in files:
        with open(path, encoding="utf-8") as f:
            src = f.read()
        rel = os.path.relpath(path, ROOT)
        for m in CALL.finditer(src):
            text = "".join(decode(b) for b in LITERAL.findall(m.group(1)))
            line = src.count("\n", 0, m.start()) + 1
            found.setdefault(text, []).append(f"{rel}:{line}")
    return found


def conversions(s):
    """The printf conversions in s, as src/core/i18n.cpp compares them."""
    out = []
    i = 0
    while i < len(s):
        if s[i] != "%":
            i += 1
            continue
        i += 1
        if i < len(s) and s[i] == "%":
            i += 1
            continue
        while i < len(s) and s[i] in "-+ #0123456789.*":
            i += 1
        spec = ""
        while i < len(s) and s[i] in "hljztL":
            spec += s[i]
            i += 1
        if i < len(s):
            spec += s[i]
            i += 1
        out.append(spec)
    return "".join(out)


def names(s):
    """The {names} in s, which the code replaces."""
    return sorted(re.findall(r"\{[a-z_]+\}", s))


def languages():
    return sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(LANG_DIR, "*.json")))


def load(code):
    with open(os.path.join(LANG_DIR, code + ".json"), encoding="utf-8") as f:
        return json.load(f, object_pairs_hook=collections.OrderedDict)


def check(strings):
    bad = 0
    print(f"{len(strings)} texts in the code")
    for code in languages():
        tr = load(code)
        missing = [s for s in strings if not tr.get(s)]
        stale = [s for s in tr if s not in strings]
        wrong = [s for s in strings if tr.get(s) and (conversions(s) != conversions(tr[s]) or names(s) != names(tr[s]))]
        print(f"{code}: {len(strings) - len(missing)} translated, {len(missing)} missing"
              + (f", {len(stale)} no longer used (update drops them)" if stale else "")
              + (f", {len(wrong)} with the wrong % codes or {{names}}" if wrong else ""))
        for s in wrong:
            print(f"  wrong % codes or {{names}}: {s!r} -> {tr[s]!r}")
        bad += len(wrong)
    return 1 if bad else 0


def update(strings):
    for code in languages():
        tr = load(code)
        out = collections.OrderedDict((s, tr.get(s, "")) for s in strings)
        dropped = [s for s in tr if s not in strings]
        with open(os.path.join(LANG_DIR, code + ".json"), "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
            f.write("\n")
        added = sum(1 for s in strings if s not in tr)
        print(f"{code}: {added} new, {len(dropped)} dropped")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    strings = extract()
    if cmd == "check":
        sys.exit(check(strings))
    elif cmd == "update":
        update(strings)
    elif cmd == "context":
        json.dump([{"text": s, "used": where} for s, where in strings.items()], sys.stdout, ensure_ascii=False, indent=1)
        print()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
