#!/usr/bin/env python3
"""Collects the launcher's translatable strings (docs/DESIGN.md 7.5).

The launcher's source language is English and the English text is the key:
RmlUi passes every text node of a document through the translation table,
and C++ passes its strings through app::tr(). This writes
data/xerces/i18n/template.json, {"English": ""} for every string found, which a
translator copies to <lang>.json and fills in. Run it after changing screens:

    python tools/i18n/extract_strings.py          # writes the template
    python tools/i18n/extract_strings.py --check  # exit 1 if it is stale

It finds: text between tags in data/xerces/ui/**/*.rml (not data bindings
{{...}}), string literals passed to tr( and to app::dialog( (title, body and
button labels), and adjacent literals joined in those calls. Strings built at
run time cannot be found; keep them out of translatable text.

Both modes also fail on a placeholder="..." attribute with text to
translate: RmlUi never translates attributes, so such placeholders are bound
from tr() through the screen's model (data-attr-placeholder).
"""

import argparse
import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "data" / "xerces" / "ui"
SRC = ROOT / "src" / "xerces_launcher"
OUT = ROOT / "data" / "xerces" / "i18n" / "template.json"

LITERAL = r'"((?:[^"\\\n]|\\.)*)"'


def unescape_cpp(text):
    return (
        text.replace(r"\"", '"').replace(r"\n", "\n").replace(r"\t", "\t").replace("\\\\", "\\")
    )


# Not launcher text: the D4 spike's mockup document, and key caps, versions
# and other tokens that stay as they are in every language.
SKIP_FILES = {UI / "home.rml"}
UNTRANSLATED = re.compile(r"^([A-Z]{1,2}|[0-9][0-9.\-a-z]*|LB|RB|Esc|Enter|DEV|BETA|xerces)$")


def keep(value):
    return bool(value) and re.search(r"[A-Za-z]", value) and not UNTRANSLATED.match(value)


def from_rml(text):
    found = set()
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    text = re.sub(r"<(style|head)\b.*?</\1>", "", text, flags=re.S)
    for match in re.finditer(r">([^<>]+)<", text):
        value = " ".join(html.unescape(match.group(1)).split())
        if "{{" in value or not keep(value):
            continue
        found.add(value)
    return found


# RmlUi translates text nodes only: a placeholder attribute is shown as
# written in every language (the pseudo-locale included, which hides it from
# the 7.5 check). Translatable placeholders are bound from tr() through the
# model (data-attr-placeholder) instead; a lower-case format token such as
# host:port may stay.
PLACEHOLDER = re.compile(r'\splaceholder="([^"]*)"')
FORMAT_TOKEN = re.compile(r"^[a-z0-9_.:/\-]+$")


def static_placeholders(text):
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    found = []
    for match in PLACEHOLDER.finditer(text):
        value = " ".join(html.unescape(match.group(1)).split())
        if keep(value) and not FORMAT_TOKEN.match(value):
            found.append(value)
    return found


def placeholder_problems():
    problems = []
    for path in sorted(UI.rglob("*.rml")):
        if path in SKIP_FILES:
            continue
        for value in static_placeholders(path.read_text(encoding="utf-8")):
            problems.append(
                f'{path.relative_to(ROOT).as_posix()}: placeholder="{value}" is never '
                "translated; bind it with data-attr-placeholder from tr()"
            )
    return problems


def call_arguments(text, name):
    """The argument text of each call to name(...), parentheses balanced."""
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        depth, i = 1, match.end()
        in_string = False
        while i < len(text) and depth:
            c = text[i]
            if in_string:
                if c == "\\":
                    i += 1
                elif c == '"':
                    in_string = False
            elif c == '"':
                in_string = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            i += 1
        yield text[match.end() : i - 1]


def literals(arguments):
    """Each run of adjacent string literals in the argument text, joined."""
    out = []
    for run in re.finditer(r"(?:" + LITERAL + r"\s*)+", arguments):
        parts = re.findall(LITERAL, run.group(0))
        out.append(unescape_cpp("".join(parts)))
    return out


def from_cpp(text):
    found = set()
    for arguments in call_arguments(text, "tr"):
        strings = literals(arguments)
        if strings and arguments.lstrip().startswith('"') and keep(strings[0]):
            found.add(strings[0])
    for arguments in call_arguments(text, "dialog"):
        for value in literals(arguments):
            if keep(value):
                found.add(value)
    return found


def collect():
    strings = set()
    for path in sorted(UI.rglob("*.rml")):
        if path not in SKIP_FILES:
            strings |= from_rml(path.read_text(encoding="utf-8"))
    for path in sorted(SRC.rglob("*.cpp")):
        strings |= from_cpp(path.read_text(encoding="utf-8"))
    return {value: "" for value in sorted(strings)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    template = json.dumps(collect(), ensure_ascii=False, indent=2) + "\n"
    problems = placeholder_problems()
    for problem in problems:
        print(problem)
    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != template:
            print(f"{OUT.relative_to(ROOT)} is stale: run tools/i18n/extract_strings.py")
            return 1
        if problems:
            return 1
        print("i18n template: ok")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(template, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}: {template.count(chr(10)) - 2} strings")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
