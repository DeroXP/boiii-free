"""The server's own privacy scrub of crash report text, and text every database can store.

The launcher redacts a report before it sends it. The server checks again that no Windows profile
path such as C:\\Users\\<name> survives: it becomes C:\\Users\\<user>. NULs and lone surrogates
are cleaned out so PostgreSQL can store every string. app.schemas runs this on each field once
its length as sent has been checked, then cuts the field back to its cap, because <user> can be
longer than the name it replaces.
"""

import re
from typing import Any

USER_PLACEHOLDER = "<user>"
# Profile folders that name nobody; kept so paths into them stay readable.
SHARED_PROFILES = frozenset({"public", "default", "default user", "all users"})

# A drive, then Users (or the old Documents and Settings), then the profile folder's name: every
# character up to the next path separator, quote or control character. JSON-escaped backslashes
# (C:\\Users\\name) match too. Over-matching only hides more. A name may start with <user>: the
# launcher ends a name at a space, so "C:\Users\John Smith" leaves it as "<user> Smith".
_NAME_END = r"\\/:*?\"<>|\x00-\x1f"
_NAME = r"((?:" + USER_PLACEHOLDER + r")?[^" + _NAME_END + r"]*)"
_PROFILE_PATH = re.compile(
    r"([A-Za-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+)" + _NAME,
    re.IGNORECASE,
)
# Proton and Linux: /home/<name> or Z:\home\<name>, where a path starts (not ui/home/menu). A
# match may start only at the first slash of a run, so a long run of slashes costs linear time.
_HOME_PATH = re.compile(
    r"((?<![\w.~\\/-])(?:[A-Za-z]:)?[\\/]+home[\\/]+)" + _NAME,
    re.IGNORECASE,
)
# Control characters, which no name, version or frame needs: tabs and line breaks become spaces,
# the rest (ESC, which starts terminal escape sequences, among them) go.
_ONE_LINE = {code: None for code in (*range(0x20), *range(0x7F, 0xA0))} | {
    ord("\t"): " ",
    ord("\n"): " ",
    ord("\r"): " ",
}


class TooDeep(ValueError):
    pass


def _replace_name(match: re.Match[str]) -> str:
    name = match.group(2)
    if not name or name == USER_PLACEHOLDER or name.strip().lower() in SHARED_PROFILES:
        return match.group(0)
    return match.group(1) + USER_PLACEHOLDER


def storable(text: str) -> str:
    """The text with lone surrogates ("\\ud800" parses as JSON but is not text) replaced by '?'.

    The length stays the same, so a length checked before still holds.
    """
    if text.isascii():
        return text
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:
        return text.encode("utf-8", "replace").decode("utf-8")
    return text


def scrub_text(text: str) -> str:
    """Profile paths with the user name replaced, NULs removed and lone surrogates replaced."""
    if "\x00" in text:
        text = text.replace("\x00", "")
    text = storable(text)
    if ":" in text:
        text = _PROFILE_PATH.sub(_replace_name, text)
    return _HOME_PATH.sub(_replace_name, text)


def one_line(text: str) -> str:
    """The text without control characters (tabs and line breaks become spaces)."""
    return text.translate(_ONE_LINE)


def scrub(value: Any, max_depth: int = 32) -> Any:
    """A copy of a parsed JSON value with every string (keys too) scrubbed. Iterative, and
    refuses nesting deeper than `max_depth` (TooDeep), so no input can exhaust the stack."""
    root: list[Any] = [None]
    stack: list[tuple[Any, list[Any] | dict[str, Any], Any, int]] = [(value, root, 0, 0)]
    while stack:
        node, parent, slot, depth = stack.pop()
        if depth > max_depth:
            raise TooDeep(f"nested more than {max_depth} levels deep")
        if isinstance(node, dict):
            copy: dict[str, Any] = {}
            parent[slot] = copy
            for key, item in node.items():
                clean_key = scrub_text(key)
                copy[clean_key] = None
                stack.append((item, copy, clean_key, depth + 1))
        elif isinstance(node, list):
            items: list[Any] = [None] * len(node)
            parent[slot] = items
            stack.extend((item, items, index, depth + 1) for index, item in enumerate(node))
        elif isinstance(node, str):
            parent[slot] = scrub_text(node)
        else:
            parent[slot] = node
    return root[0]
