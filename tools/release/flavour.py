"""One player guide for betas and stable releases.

docs/TESTERS.md is README.txt in every zip and the website's guide. A few of
its lines are only true of a beta (the private link, the tester channel); they
sit in blocks with the stable wording next to them:

    <!-- beta -->
    The private link comes with the zip's SHA-256.
    <!-- stable -->
    The download page shows the zip's SHA-256.
    <!-- end -->

Each marker is a line of its own; either part may be empty. pick() keeps the
part for the release and drops the markers. A version with a pre-release part
(1.0.0-rc.1, 0.1.0-beta.27) is a beta; 1.0.0 is stable.

  python -I tools/release/flavour.py <in.md> <out.txt> --version <version>
      writes the version's text with CRLF line ends (release.ps1)
  python -I tools/release/flavour.py --self-test
"""
import argparse
import re
import sys

MARKERS = {"<!-- beta -->": "beta", "<!-- stable -->": "stable", "<!-- end -->": "end"}


class Problem(Exception):
    pass


def is_stable(version):
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", version):
        raise Problem(f"not a version: {version!r}")
    return "-" not in version


def pick(text, stable):
    """The text for a stable release or a beta, without the markers."""
    out = []
    part = None  # None outside a block, else "beta" or "stable"
    for number, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        marker = MARKERS.get(line.strip())
        if marker == "beta":
            if part is not None:
                raise Problem(f"line {number}: <!-- beta --> inside a block")
            part = "beta"
        elif marker == "stable":
            if part != "beta":
                raise Problem(f"line {number}: <!-- stable --> without <!-- beta --> before it")
            part = "stable"
        elif marker == "end":
            if part is None:
                raise Problem(f"line {number}: <!-- end --> outside a block")
            part = None
        elif part is None or (part == "stable") == stable:
            out.append(line)
    if part is not None:
        raise Problem("a block has no <!-- end -->")
    return "\n".join(out)


def self_test():
    doc = "a\n<!-- beta -->\nb\n<!-- stable -->\ns\n<!-- end -->\nc\n<!-- beta -->\nonly beta\n<!-- end -->\nd"
    checks = [
        (pick(doc, False) == "a\nb\nc\nonly beta\nd", "beta text"),
        (pick(doc, True) == "a\ns\nc\nd", "stable text"),
        (pick("x\r\ny", True) == "x\ny", "CRLF"),
        (is_stable("1.0.0") and not is_stable("1.0.0-rc.1") and not is_stable("0.1.0-beta.27"), "versions"),
    ]
    for bad in ("<!-- end -->", "<!-- beta -->\nx", "<!-- stable -->\n<!-- end -->",
                "<!-- beta -->\n<!-- beta -->\n<!-- end -->"):
        try:
            pick(bad, True)
            checks.append((False, "accepted " + repr(bad)))
        except Problem:
            pass
    failed = [name for ok, name in checks if not ok]
    print("flavour self-test:", "FAILED " + ", ".join(failed) if failed else "ok")
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", nargs="?")
    p.add_argument("target", nargs="?")
    p.add_argument("--version")
    p.add_argument("--self-test", action="store_true")
    a = p.parse_args()
    if a.self_test:
        return self_test()
    if not (a.source and a.target and a.version):
        p.error("needs <in.md> <out.txt> --version <version>")
    try:
        text = pick(open(a.source, encoding="utf-8").read(), is_stable(a.version))
    except Problem as e:
        print(f"{a.source}: {e}")
        return 1
    with open(a.target, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
