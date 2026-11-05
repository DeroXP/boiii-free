#!/usr/bin/env python3
"""Generates the C / Signal colour tokens from data/xerces/ui/tokens.json.

docs/DESIGN.md 7.2: the tokens live in one file and generate the launcher's
styles and the in-game Lua colour table.

- data/xerces/ui/base.rcss: the block between "tokens:begin" and
  "tokens:end" (body for dark, body.theme-light, body.theme-contrast).
- data/xerces/lua/xerces_tokens.lua: XercesTokens[theme][token] = {r, g, b}
  in 0..1, for the in-game menu (7.6, not built yet).

    python tools/ui/tokens.py           # write both
    python tools/ui/tokens.py --check   # exit 1 if either is stale (CI)
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOKENS = ROOT / "data" / "xerces" / "ui" / "tokens.json"
RCSS = ROOT / "data" / "xerces" / "ui" / "base.rcss"
LUA = ROOT / "data" / "xerces" / "lua" / "xerces_tokens.lua"
BEGIN = "/* tokens:begin - generated from tokens.json by tools/ui/tokens.py; do not edit */"
END = "/* tokens:end */"
SELECTORS = {"dark": "body", "light": "body.theme-light", "contrast": "body.theme-contrast"}


def rcss_block(themes):
    lines = [BEGIN]
    for theme, selector in SELECTORS.items():
        lines.append(selector)
        lines.append("{")
        for name, value in themes[theme].items():
            lines.append(f"\t--{name}: {value};")
        lines.append("}")
    lines.append(END)
    return "\n".join(lines)


def lua_table(themes):
    out = [
        "-- C / Signal colours for the in-game Xerces menu (docs/DESIGN.md 7.2, 7.6).",
        "-- Generated from data/xerces/ui/tokens.json by tools/ui/tokens.py; do not edit.",
        "XercesTokens = {",
    ]
    for theme in SELECTORS:
        out.append(f"\t{theme} = {{")
        for name, value in themes[theme].items():
            r, g, b = (int(value[i : i + 2], 16) / 255 for i in (1, 3, 5))
            key = name.replace("-", "_")
            out.append(f"\t\t{key} = {{ {r:.4f}, {g:.4f}, {b:.4f} }},")
        out.append("\t},")
    out.append("}")
    return "\n".join(out) + "\n"


def render():
    data = json.loads(TOKENS.read_text(encoding="utf-8"))
    themes = data["themes"]
    for theme in SELECTORS:
        for name, value in themes[theme].items():
            if not re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
                raise SystemExit(f"tokens.json: {theme}.{name} is not #RRGGBB: {value}")
    rcss = RCSS.read_text(encoding="utf-8")
    block = rcss_block(themes)
    if BEGIN in rcss and END in rcss:
        start = rcss.index(BEGIN)
        end = rcss.index(END) + len(END)
        rcss = rcss[:start] + block + rcss[end:]
    else:
        raise SystemExit(f"{RCSS.relative_to(ROOT)} has no tokens:begin/tokens:end block")
    return rcss, lua_table(themes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rcss, lua = render()
    if args.check:
        stale = []
        if RCSS.read_text(encoding="utf-8") != rcss:
            stale.append(str(RCSS.relative_to(ROOT)))
        if not LUA.exists() or LUA.read_text(encoding="utf-8") != lua:
            stale.append(str(LUA.relative_to(ROOT)))
        if stale:
            print("stale: " + ", ".join(stale) + " (run tools/ui/tokens.py)")
            return 1
        print("tokens: ok")
        return 0
    RCSS.write_text(rcss, encoding="utf-8", newline="\n")
    LUA.parent.mkdir(parents=True, exist_ok=True)
    LUA.write_text(lua, encoding="utf-8", newline="\n")
    print("wrote base.rcss tokens and xerces_tokens.lua")
    return 0


if __name__ == "__main__":
    sys.exit(main())
