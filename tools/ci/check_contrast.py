#!/usr/bin/env python3
"""Checks the launcher's colour tokens for contrast (docs/DESIGN.md 7.2, 7.5).

Reads the token blocks of data/xerces/ui/base.rcss (body for dark,
body.theme-light, body.theme-contrast) and checks every pair of text and
background tokens the screens use against WCAG 2's 4.5:1 for text. Exits 1
and names the pair when one falls short.

    python tools/ci/check_contrast.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RCSS = ROOT / "data" / "xerces" / "ui" / "base.rcss"

# (text token, background token): what the screens actually put together.
PAIRS = [
    ("text", "bg"),
    ("text", "surface"),
    ("text", "surface2"),
    ("text2", "bg"),
    ("text2", "surface"),
    ("text2", "surface2"),
    ("accent-text", "bg"),
    ("accent-text", "surface"),
    ("on-accent", "accent"),
    ("bg", "text"),  # hover inverts outline buttons
]
MINIMUM = 4.5


def blocks(text):
    """Selector -> {token: #RRGGBB} for the theme blocks."""
    out = {}
    for match in re.finditer(r"(body(?:\.theme-[a-z]+)?)\s*\{([^}]*)\}", text):
        tokens = dict(re.findall(r"--([a-z0-9-]+)\s*:\s*(#[0-9A-Fa-f]{6})\s*;", match.group(2)))
        if tokens:
            out.setdefault(match.group(1), {}).update(tokens)
    return out


def luminance(colour):
    channels = [int(colour[i : i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def ratio(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def main():
    themes = blocks(RCSS.read_text(encoding="utf-8"))
    dark = themes.get("body")
    if not dark:
        print(f"{RCSS}: no token block on body")
        return 1
    failures = 0
    for name, selector in (("dark", "body"), ("light", "body.theme-light"), ("contrast", "body.theme-contrast")):
        tokens = dict(dark)
        tokens.update(themes.get(selector, {}))
        for fore, back in PAIRS:
            if fore not in tokens or back not in tokens:
                print(f"{name}: token --{fore} or --{back} is missing")
                failures += 1
                continue
            value = ratio(tokens[fore], tokens[back])
            if value < MINIMUM:
                print(f"{name}: --{fore} {tokens[fore]} on --{back} {tokens[back]} is {value:.2f}:1, below {MINIMUM}:1")
                failures += 1
    if failures:
        return 1
    print(f"contrast: ok ({len(PAIRS)} pairs in 3 themes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
