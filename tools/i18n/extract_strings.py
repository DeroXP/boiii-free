#!/usr/bin/env python3
"""Collects the launcher's translatable strings (docs/DESIGN.md 7.5).

The launcher's source language is English and the English text is the key:
RmlUi passes every text node of a document through the translation table,
and C++ passes its strings through app::tr(). This writes
data/xerces/i18n/template.json, {"English": ""} for every string found, which a
translator copies to <lang>.json and fills in. Run it after changing screens:

    python tools/i18n/extract_strings.py          # writes the template
    python tools/i18n/extract_strings.py --check  # exit 1 if it is stale

It finds, the way xgettext does with keywords:

- text between tags in data/xerces/ui/**/*.rml (not data bindings {{...}});
- in src/xerces_launcher, every string literal in a tr( call, so both arms of
  `tr(n == 1 ? "mod" : "mods")` count, and the title, body and button labels
  of app::dialog( and the titles of the app's pickers;
- everywhere in src/xerces_launcher and src/xerces_core, every literal in a
  ui_text( call: xerces_core/ui_text.hpp marks text that a screen
  translates later, such as a message the services hand on;
- the arguments of the helpers in HELPERS that pass them to tr() (a screen's
  row label, a core diagnostic a screen shows), and every literal in the
  functions in TEXT_FUNCTIONS, whose results the screens pass to tr() (label
  tables, the core's messages for the player).

Adjacent literals are joined. A literal compared with == or != is a value,
and one joined to something else with + is a fragment of a string built at
run time: neither is text. Build such text from fixed parts that go through
tr() one by one. --check fails when a helper or function named in the
tables is gone from its file, so a rename cannot drop strings silently.

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
CORE = ROOT / "src" / "xerces_core"
OUT = ROOT / "data" / "xerces" / "i18n" / "template.json"

LITERAL = r'"((?:[^"\\\n]|\\.)*)"'

# Every literal in these calls is text: tr() in the launcher, the ui_text()
# marker everywhere.
LAUNCHER_TEXT_CALLS = ("tr", "ui_text")
CORE_TEXT_CALLS = ("ui_text",)
# Launcher calls whose arguments at these positions (from 0) are text: the
# app's dialog (title, body, buttons; not its callback) and its pickers.
ARGUMENT_CALLS = {
    "dialog": (0, 1, 2),
    "pick_folder": (0,),
    "pick_open_file": (0,),
    "pick_save_file": (0,),
}
# Calls in one file whose arguments at these positions are text that reaches
# tr(): file -> {name: positions}.
HELPERS = {
    # A screen's helpers that translate a parameter.
    "src/xerces_launcher/views/diagnostics.cpp": {"row": (0,)},
    "src/xerces_launcher/views/mods.cpp": {"field": (0,)},
    "src/xerces_launcher/views/packs.cpp": {"add": (1,), "show_import": (1,), "store": (1,)},
    # Core diagnostics whose message a screen translates: First run's import
    # warnings, Servers' launch problems.
    "src/xerces_core/first_run_import.cpp": {"warning": (2,)},
    "src/xerces_core/launch.cpp": {"error": (2,)},
}
# Functions in one file whose every literal is text the screens pass to tr().
TEXT_FUNCTIONS = {
    "src/xerces_launcher/views/diagnostics.cpp": ("phase_label",),
    "src/xerces_launcher/views/first_run.cpp": ("verdict_name",),
    "src/xerces_launcher/views/home.cpp": ("mode_name",),
    "src/xerces_launcher/views/mods.cpp": ("mode_name", "source_name"),
    "src/xerces_launcher/views/packs.cpp": ("mode_name",),
    "src/xerces_launcher/views/servers.cpp": ("mode_name",),
    "src/xerces_core/compat.cpp": ("preflight_label",),
    "src/xerces_core/journal.cpp": ("safe_mode_message", "setting_on_sentence", "setting_name"),
    "src/xerces_core/mods_edit.cpp": ("mods_capabilities",),
    "src/xerces_core/packs_store.cpp": ("pack_name_problem",),
    "src/xerces_core/servers_book.cpp": ("port_problem", "server_address_problem"),
}


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


def is_char_literal(text, i):
    """A ' that opens a character literal (not a digit separator)."""
    return text[i] == "'" and not (i and (text[i - 1].isalnum() or text[i - 1] == "_"))


def skip_literal(text, i):
    """The index after the string or character literal that starts at i."""
    quote = text[i]
    i += 1
    while i < len(text) and text[i] != quote and text[i] != "\n":
        i += 2 if text[i] == "\\" else 1
    return i + 1


def strip_comments(text):
    """The C++ source without // and /* */ comments; literals stay as they are."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == '"' or is_char_literal(text, i):
            end = skip_literal(text, i)
            out.append(text[i:end])
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = len(text) if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = len(text) if end < 0 else end + 2
            out.append(" ")
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def balanced_end(text, i, opening, closing):
    """The index after the bracket that closes the one before i."""
    depth = 1
    while i < len(text) and depth:
        if text[i] == '"' or is_char_literal(text, i):
            i = skip_literal(text, i)
            continue
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
        i += 1
    return i


def call_arguments(text, name):
    """The argument text of each call to name(...), parentheses balanced."""
    for match in re.finditer(r"(?<!\w)" + re.escape(name) + r"\s*\(", text):
        end = balanced_end(text, match.end(), "(", ")")
        yield text[match.end() : end - 1]


def split_arguments(arguments):
    """The top-level comma-separated arguments of a call's argument text."""
    parts, depth, start, i = [], 0, 0, 0
    while i < len(arguments):
        c = arguments[i]
        if c == '"' or is_char_literal(arguments, i):
            i = skip_literal(arguments, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(arguments[start:i])
            start = i + 1
        i += 1
    parts.append(arguments[start:])
    return parts


def literals(code):
    """Each run of adjacent string literals in code that is text, joined: not
    compared (== or !=) and not joined to anything else with +."""
    out = []
    for run in re.finditer(r"(?:" + LITERAL + r"\s*)+", code):
        before = code[: run.start()].rstrip()
        after = code[run.end() :].lstrip()
        if before.endswith(("==", "!=", "+")) or after.startswith(("==", "!=", "+")):
            continue
        parts = re.findall(LITERAL, run.group(0))
        out.append(unescape_cpp("".join(parts)))
    return out


def function_body(text, name):
    """The body of the function named name defined in text, or None."""
    for match in re.finditer(r"(?<!\w)" + re.escape(name) + r"\s*\(", text):
        end = balanced_end(text, match.end(), "(", ")")
        # A definition: the parameter list is followed by its body (after
        # const or a trailing return type), a call by anything else.
        tail = re.match(r"\s*(?:const\s*)?(?:->\s*[\w:<>*& ]+)?\{", text[end:])
        if tail:
            start = end + tail.end()
            return text[start : balanced_end(text, start, "{", "}") - 1]
    return None


def from_cpp(text, launcher=True, helpers=None, functions=()):
    """The strings of one C++ file, with its HELPERS and TEXT_FUNCTIONS
    entries. Raises LookupError when one of those is not in the file."""
    text = strip_comments(text)
    found = []
    for name in LAUNCHER_TEXT_CALLS if launcher else CORE_TEXT_CALLS:
        for arguments in call_arguments(text, name):
            found += literals(arguments)
    calls = dict(ARGUMENT_CALLS) if launcher else {}
    for name, positions in (helpers or {}).items():
        if not re.search(r"(?<!\w)" + re.escape(name) + r"\s*\(", text):
            raise LookupError(f"no call to {name}()")
        calls[name] = positions
    for name, positions in calls.items():
        for arguments in call_arguments(text, name):
            parts = split_arguments(arguments)
            for position in positions:
                if position < len(parts):
                    found += literals(parts[position])
    for name in functions:
        body = function_body(text, name)
        if body is None:
            raise LookupError(f"no function {name}()")
        found += literals(body)
    return {value for value in found if keep(value)}


def collect():
    strings = set()
    for path in sorted(UI.rglob("*.rml")):
        if path not in SKIP_FILES:
            strings |= from_rml(path.read_text(encoding="utf-8"))
    sources = [(path, True) for path in sorted(SRC.rglob("*.cpp"))]
    sources += [(path, False) for path in sorted(CORE.rglob("*.?pp")) if path.name != "ui_text.hpp"]
    named = set(HELPERS) | set(TEXT_FUNCTIONS)
    for path, launcher in sources:
        name = path.relative_to(ROOT).as_posix()
        named.discard(name)
        try:
            strings |= from_cpp(
                path.read_text(encoding="utf-8"), launcher, HELPERS.get(name), TEXT_FUNCTIONS.get(name, ())
            )
        except LookupError as error:
            raise SystemExit(f"{name}: {error}; update HELPERS or TEXT_FUNCTIONS in tools/i18n/extract_strings.py")
    if named:
        raise SystemExit(f"{', '.join(sorted(named))}: not found; update HELPERS or TEXT_FUNCTIONS in tools/i18n/extract_strings.py")
    return {value: "" for value in sorted(strings)}


def self_test():
    """The rules above on a small sample; --check runs it first."""
    sample = r"""
        const char *verdict(int v) {
          switch (v) { case 1: return "Supported"; }
          return x == "id" ? "Unknown build" : "Other";
        }
        void f() {
          owner_->tr(n == 1 ? "mod" : "mods"); // "Not text"
          tr(mode == "zm" ? "Zombies" : "Campaign");
          tr("Two " "parts");
          auto row = [&](const std::string &name, std::string value) {
            out.push_back(owner_->tr(name));
          };
          row("Exe size", "12 MB");
          owner_->dialog("Title", body + owner_->tr("Body part"), {"Yes", "No"},
                         [this](int) { act("launcher.event"); });
          owner_->pick_folder("Pick a folder");
          log_info("launcher.x", "Not text either");
          notify(owner_->tr("The pack" " is ") + "a fragment" + name);
          const char *quote = '"' == c ? "Quote" : "";
        }
    """
    got = from_cpp(sample, True, {"row": (0,)}, ("verdict",))
    want = {"Supported", "Unknown build", "Other", "mod", "mods", "Zombies", "Campaign",
            "Two parts", "Exe size", "Title", "Body part", "Yes", "No", "Pick a folder",
            "The pack is "}
    if got != want:
        raise SystemExit(f"extract_strings self-test: got {sorted(got)}, want {sorted(want)}")
    for helpers, functions in (({"field": (0,)}, ()), (None, ("label",))):
        try:
            from_cpp(sample, True, helpers, functions)
        except LookupError:
            continue
        raise SystemExit("extract_strings self-test: a missing helper or function was not reported")
    core = r"""
        std::string problem(std::string_view name) {
          if (name.empty()) { return "Give the pack a name."; }
          return "Too long: " + std::string(name);
        }
        void g() {
          diags.warning("first_run.import.x", path, "Skipped: not a file.");
          out.message = ui_text("The game is already running.");
          tr("Not in the core");
        }
    """
    got = from_cpp(core, False, {"warning": (2,)}, ("problem",))
    want = {"Give the pack a name.", "Skipped: not a file.", "The game is already running."}
    if got != want:
        raise SystemExit(f"extract_strings self-test (core): got {sorted(got)}, want {sorted(want)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        self_test()
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
