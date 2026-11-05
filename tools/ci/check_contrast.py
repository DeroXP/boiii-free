#!/usr/bin/env python3
"""Checks the launcher's colour tokens for contrast (docs/DESIGN.md 7.2, 7.5).

Reads the themes in data/xerces/ui/tokens.json (the source of base.rcss's
token block) and checks every pair of text and background tokens the screens
use (contrast_pairs) against WCAG 2's 4.5:1 for text. Exits 1 and names the
pair when one falls short.

    python tools/ci/check_contrast.py
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOKENS = ROOT / "data" / "xerces" / "ui" / "tokens.json"

MINIMUM = 4.5


def luminance(colour):
    channels = [int(colour[i : i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def ratio(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def main():
    data = json.loads(TOKENS.read_text(encoding="utf-8"))
    pairs = data["contrast_pairs"]
    failures = 0
    for name, tokens in data["themes"].items():
        for fore, back in pairs:
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
    print(f"contrast: ok ({len(pairs)} pairs in {len(data['themes'])} themes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
