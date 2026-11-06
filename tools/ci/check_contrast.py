#!/usr/bin/env python3
"""Checks the launcher's colour tokens for contrast (docs/DESIGN.md 7.2, 7.5).

Reads the themes in data/xerces/ui/tokens.json (the source of base.rcss's
token block) and checks text against its background in every theme against
WCAG 2's 4.5:1 for text:

- the pairs listed in contrast_pairs (text on the page and panel colours,
  which the stylesheets reach through inheritance), and
- every rule in the launcher's stylesheets (base.rcss, views/*.rcss) that
  sets a background token: its text colour is the rule's own color, or the
  color of the same selector without its states (".primary-button" for
  ".primary-button:hover"), so a hover or focus colour is checked against
  the text it keeps.

Exits 1 and names the pair (and the rule) when one falls short.

    python tools/ci/check_contrast.py
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "data" / "xerces" / "ui"
TOKENS = UI / "tokens.json"

MINIMUM = 4.5

# States an element passes through; the selector without them is the
# element's resting rule, where its text colour usually is.
STATES = re.compile(r":(hover|focus-visible|focus|active|checked|disabled)\b")
VAR = re.compile(r"^var\(--([A-Za-z0-9-]+)\)$")


def luminance(colour):
    channels = [int(colour[i : i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def ratio(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def rules(text):
    """(selector, {property: value}) for each rule, @media blocks included."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = []
    prelude_start = 0
    i = 0
    while i < len(text):
        c = text[i]
        if c == "{":
            prelude = text[prelude_start:i].strip()
            if prelude.startswith("@"):
                prelude_start = i + 1  # its rules follow; its } is skipped below
            else:
                end = text.index("}", i)
                declarations = {}
                for part in text[i + 1 : end].split(";"):
                    name, colon, value = part.partition(":")
                    if colon:
                        declarations[name.strip().lower()] = value.strip()
                for selector in prelude.split(","):
                    out.append((" ".join(selector.split()), declarations))
                i = end
                prelude_start = end + 1
        elif c in "};":
            prelude_start = i + 1
        i += 1
    return out


def token(value):
    match = VAR.match(value or "")
    return match.group(1) if match else None


def stylesheet_pairs():
    """(fore token, back token, where) for each rule that sets a background."""
    sheets = [UI / "base.rcss"] + sorted((UI / "views").glob("*.rcss"))
    colour_of = {}
    found = []
    for sheet in sheets:
        for selector, declarations in rules(sheet.read_text(encoding="utf-8")):
            if "color" in declarations:
                colour_of[selector] = declarations["color"]
            back = token(declarations.get("background-color") or declarations.get("background"))
            if back:
                found.append((selector, declarations, back, sheet.name))
    pairs = []
    for selector, declarations, back, sheet in found:
        fore = token(declarations.get("color"))
        if fore is None:
            fore = token(colour_of.get(STATES.sub("", selector)))
        if fore is not None:
            pairs.append((fore, back, f"{sheet}: {selector}"))
    return pairs


def main():
    data = json.loads(TOKENS.read_text(encoding="utf-8"))
    pairs = [(fore, back, "contrast_pairs") for fore, back in data["contrast_pairs"]]
    pairs += stylesheet_pairs()
    failures = 0
    for name, tokens in data["themes"].items():
        for fore, back, where in pairs:
            if fore not in tokens or back not in tokens:
                print(f"{name}: token --{fore} or --{back} is missing ({where})")
                failures += 1
                continue
            value = ratio(tokens[fore], tokens[back])
            if value < MINIMUM:
                print(
                    f"{name}: --{fore} {tokens[fore]} on --{back} {tokens[back]} is "
                    f"{value:.2f}:1, below {MINIMUM}:1 ({where})"
                )
                failures += 1
    if failures:
        return 1
    distinct = len({(fore, back) for fore, back, _ in pairs})
    print(f"contrast: ok ({distinct} pairs in {len(data['themes'])} themes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
