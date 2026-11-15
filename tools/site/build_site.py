#!/usr/bin/env python3
"""Builds Xerces' website (site/README.md) into a folder of plain files.

    python -I tools/site/build_site.py                 # writes build/site
    python -I tools/site/build_site.py --out <folder>
    python -I tools/site/build_site.py --check         # builds into a temporary folder, exit 1 on a problem

The pages come from the repository itself, so a release updates the site from
the same text its zip carries: the landing page and the download page from
site/templates and site/releases.json, the guide from docs/TESTERS.md, the
known issues from docs/KNOWN_ISSUES.md and the changelog from CHANGELOG.md.
The fonts are the launcher's (data/xerces/fonts, SIL OFL). No dependencies:
the Markdown here is the small subset these documents use.
"""
import argparse
import html
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "site"

NAV = [
    ("download.html", "Download"),
    ("index.html#features", "Features"),
    ("guide.html", "Guide"),
    ("known-issues.html", "Known issues"),
    ("changelog.html", "Changelog"),
]


# ------------------------------------------------------------------ markdown
def slug(text):
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text).lower()
    text = re.sub(r"[`*_]", "", text)
    text = re.sub(r"[^a-z0-9]+", "-", text).strip("-")
    return text or "section"


def inline(text):
    """Escapes and formats one run of text: code, links, bold, italics."""
    out = []
    parts = re.split(r"(`[^`]+`)", text)
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) > 1:
            out.append("<code>" + html.escape(part[1:-1], quote=False) + "</code>")
            continue
        s = html.escape(part, quote=False)
        s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)",
                   lambda m: '<a href="%s">%s</a>' % (html.escape(m.group(2)), m.group(1)), s)
        s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
        s = re.sub(r"(?<![\w*])\*([^*\s][^*]*?)\*(?![\w*])", r"<em>\1</em>", s)
        s = re.sub(r"(?<![\w])_([^_\s][^_]*?)_(?![\w])", r"<em>\1</em>", s)
        out.append(s)
    return "".join(out)


LIST_ITEM = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def render_list(lines):
    """lines: a list block; the first line is an item at the base indent."""
    first = LIST_ITEM.match(lines[0])
    base = indent_of(lines[0])
    ordered = first.group(2)[0].isdigit()
    items = []
    for line in lines:
        m = LIST_ITEM.match(line)
        if m and indent_of(line) == base:
            items.append([m.group(3)])
        else:
            # continuation: dedent by the item's text column
            col = base + 2 if not ordered else base + 3
            items[-1].append(line[col:] if line.strip() and indent_of(line) >= col else line.strip())
    tag = "ol" if ordered else "ul"
    out = ["<%s>" % tag]
    for item in items:
        # a single paragraph item renders inline; one with nested blocks renders as blocks
        has_blocks = any(LIST_ITEM.match(l) for l in item[1:]) or any(not l.strip() for l in item[1:])
        if has_blocks:
            body = render_blocks(item)
            body = re.sub(r"^<p>(.*?)</p>", r"\1", body, count=1, flags=re.S)
            out.append("<li>" + body + "</li>")
        else:
            out.append("<li>" + inline(" ".join(l.strip() for l in item)) + "</li>")
    out.append("</%s>" % tag)
    return "\n".join(out)


def render_table(lines):
    rows = [[c.strip() for c in l.strip().strip("|").split("|")] for l in lines]
    head, body = rows[0], rows[2:]
    out = ["<table><thead><tr>"]
    out += ["<th>%s</th>" % inline(c) for c in head]
    out.append("</tr></thead><tbody>")
    for r in body:
        out.append("<tr>" + "".join("<td>%s</td>" % inline(c) for c in r) + "</tr>")
    out.append("</tbody></table>")
    return "".join(out)


def render_blocks(lines, headings=None, shift=0):
    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        if line.startswith("```"):
            j = i + 1
            code = []
            while j < n and not lines[j].startswith("```"):
                code.append(lines[j])
                j += 1
            out.append("<pre><code>" + html.escape("\n".join(code), quote=False) + "</code></pre>")
            i = j + 1
            continue
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            level = min(6, len(m.group(1)) + shift)
            text = inline(m.group(2).strip())
            hid = slug(m.group(2))
            if headings is not None:
                base = hid
                k = 2
                while hid in {h[1] for h in headings}:
                    hid = "%s-%d" % (base, k)
                    k += 1
                headings.append((level, hid, text))
            out.append('<h%d id="%s">%s</h%d>' % (level, hid, text, level))
            i += 1
            continue
        if line.lstrip().startswith("|") and i + 1 < n and re.match(r"^\s*\|?\s*:?-+", lines[i + 1]):
            j = i
            while j < n and lines[j].lstrip().startswith("|"):
                j += 1
            out.append(render_table(lines[i:j]))
            i = j
            continue
        if LIST_ITEM.match(line):
            base = indent_of(line)
            j = i + 1
            while j < n:
                l = lines[j]
                if not l.strip():
                    # a blank line ends the list unless the next line still belongs to it
                    if j + 1 < n and (indent_of(lines[j + 1]) > base or
                                      (LIST_ITEM.match(lines[j + 1]) and indent_of(lines[j + 1]) == base)):
                        j += 1
                        continue
                    break
                if indent_of(l) < base or (indent_of(l) == base and not LIST_ITEM.match(l)):
                    break
                j += 1
            block = lines[i:j]
            while block and not block[-1].strip():
                block.pop()
            out.append(render_list(block))
            i = j
            continue
        if line.startswith(">"):
            j = i
            quote = []
            while j < n and lines[j].startswith(">"):
                quote.append(lines[j][1:].lstrip())
                j += 1
            out.append("<blockquote>" + render_blocks(quote) + "</blockquote>")
            i = j
            continue
        j = i
        para = []
        while j < n and lines[j].strip() and not LIST_ITEM.match(lines[j]) and not lines[j].startswith(("#", "```", ">")):
            para.append(lines[j].strip())
            j += 1
        out.append("<p>" + inline(" ".join(para)) + "</p>")
        i = j
    return "\n".join(out)


def markdown_page(path, drop_title=True):
    text = Path(path).read_text(encoding="utf-8")
    lines = text.replace("\r\n", "\n").split("\n")
    title = ""
    if drop_title and lines and lines[0].startswith("# "):
        title = lines[0][2:].strip()
        lines = lines[1:]
    headings = []
    body = render_blocks(lines, headings)
    return title, body, headings


# --------------------------------------------------------------------- pages
def nav_html(current, root):
    out = []
    for i, (href, label) in enumerate(NAV, 1):
        cur = ' aria-current="page"' if href == current else ""
        out.append('      <a href="%s%s"%s><span class="n">%02d</span>%s</a>' % (root, href, cur, i, label))
    return "\n".join(out)


def human_size(n):
    return "%.1f MB" % (n / 1_000_000)


def channel_card(name, data, repo):
    if not data:
        return ""
    state = data.get("state", "")
    title = "Stable" if name == "stable" else "Beta"
    primary = " primary" if name == "stable" else ""
    if state == "planned":
        return """      <article class="channel%s">
        <div class="top"><h3>%s</h3><span class="state">In preparation</span></div>
        <p>%s</p>
        <dl><dt>Version</dt><dd>%s</dd><dt>Status</dt><dd>Being prepared from the betas</dd></dl>
        <a class="btn" href="changelog.html">What it will have</a>
      </article>""" % (primary, title, html.escape(data.get("summary", "")), html.escape(data["version"]))
    rows = [("Version", html.escape(data["version"]), ""), ("Released", html.escape(data.get("date", "")), "")]
    if data.get("size_bytes"):
        rows.append(("Size", human_size(data["size_bytes"]) + " zip", ""))
    if data.get("sha256"):
        rows.append(("SHA-256", html.escape(data["sha256"]), ' class="hash"'))
    dl = "".join("<dt>%s</dt><dd%s>%s</dd>" % (k, cls, v) for k, v, cls in rows)
    if data.get("url"):
        action = """<a class="btn-launch" href="%s">
          <span><span class="big">Download %s</span><span class="small">%s &middot; Windows 10/11 64-bit</span></span>
          <span class="arrow" aria-hidden="true">&darr;</span>
        </a>""" % (html.escape(data["url"]), html.escape(data["version"]), html.escape(data.get("file", "")))
    else:
        action = "<p>%s</p>" % html.escape(data.get("access", ""))
    return """      <article class="channel%s">
        <div class="top"><h3>%s</h3><span class="state live">Current</span></div>
        <dl>%s</dl>
        %s
      </article>""" % (primary, title, dl, action)


def cta(releases):
    stable = releases.get("stable") or {}
    beta = releases.get("beta") or {}
    if stable.get("state") == "live":
        return "Download " + stable["version"], "Stable &middot; Windows 10/11 64-bit"
    if beta:
        return "Get Xerces", "Beta %s &middot; stable %s in preparation" % (
            html.escape(beta["version"]), html.escape(stable.get("version", "")))
    return "Get Xerces", "Windows 10/11 64-bit"


def download_body(releases, repo):
    cards = "\n".join(channel_card(c, releases.get(c), repo) for c in ("stable", "beta"))
    hist = []
    for h in releases.get("history", []):
        sha = '<code>%s</code>' % html.escape(h["sha256"]) if h.get("sha256") else "&mdash;"
        hist.append("<tr><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            html.escape(h["version"]), html.escape(h.get("date", "")), sha))
    return """<section class="page-head"><div class="wrap">
  <div class="label">Download</div>
  <h1>Get Xerces.</h1>
  <p>One zip, no installer. Check it, unzip it into a folder of its own, start Xerces.exe.</p>
</div></section>
<section class="section"><div class="wrap">
  <div class="channels">
%s
  </div>
  <p class="notice"><strong>Not code-signed yet.</strong> Windows SmartScreen may say it protected your PC. Check the zip's SHA-256 below first; if it matches, choose "More info", then "Run anyway".</p>
</div></section>
<section class="section" id="verify"><div class="wrap prose">
  <h2>Check a download</h2>
  <p>In PowerShell, in the folder with the zip, run this with your zip's name and compare what it prints with the SHA-256 on this page. If they differ, do not use the zip, and tell us.</p>
  <pre><code>Get-FileHash .\\%s</code></pre>
  <p><code>SHA256SUMS.txt</code> inside the zip lists every file's hash, to check later that nothing in the folder changed. It came in the same zip, so only the zip's own hash shows the zip is the one we made.</p>
  <h2>What you need</h2>
  <ul>
    <li>Windows 10 or 11, 64-bit.</li>
    <li>Black Ops III on Steam, installed, at a build Xerces supports (Diagnostics says which).</li>
    <li>Steam running and signed in with the account that owns the game.</li>
    <li>Nothing else: no installer, no runtimes, no admin rights.</li>
  </ul>
  <h2>Earlier releases</h2>
  <table><thead><tr><th>Version</th><th>Date</th><th>SHA-256</th></tr></thead><tbody>%s</tbody></table>
  <h2>Build it yourself</h2>
  <p>Every release names the commit it was built from. The <a href="%s">source</a> builds with the pinned tools in its README; the result can be compared file by file.</p>
</div></section>""" % (cards, html.escape((releases.get("beta") or {}).get("file", "xerces-<version>.zip")),
                        "".join(hist), html.escape(repo))


def doc_body(label, title, lede, body, headings):
    toc = "\n".join('<a href="#%s">%s</a>' % (hid, text) for level, hid, text in headings if level == 2)
    return """<section class="page-head"><div class="wrap">
  <div class="label">%s</div>
  <h1>%s</h1>
  %s
</div></section>
<div class="wrap doc">
  <nav class="toc" aria-label="On this page">%s</nav>
  <article class="prose">
%s
  </article>
</div>""" % (label, html.escape(title), ("<p>%s</p>" % lede) if lede else "", toc, body)


def changelog_body(path):
    text = Path(path).read_text(encoding="utf-8").replace("\r\n", "\n")
    lines = text.split("\n")
    if lines and lines[0].startswith("# "):
        lines = lines[1:]
    # one block per "## [version] - date"
    blocks = []
    current = None
    intro = []
    for line in lines:
        m = re.match(r"^##\s+\[([^\]]+)\](?:\s*-\s*(\S+))?", line)
        if m:
            current = {"version": m.group(1), "date": m.group(2) or "", "lines": []}
            blocks.append(current)
        elif current:
            current["lines"].append(line)
        else:
            intro.append(line)
    out = [render_blocks(intro)]
    for b in blocks:
        hid = slug(b["version"])
        out.append('<section class="release" id="%s"><h2>%s</h2><div class="date">%s</div>%s</section>' % (
            hid, html.escape(b["version"]), html.escape(b["date"]), render_blocks(b["lines"], shift=1)))
    headings = [(2, slug(b["version"]), html.escape(b["version"])) for b in blocks]
    return "\n".join(out), headings


def page(layout, title, description, current, content, releases, root=""):
    beta = releases.get("beta") or {}
    stable = releases.get("stable") or {}
    live_stable = stable.get("state") == "live"
    version = stable["version"] if live_stable else beta.get("version", "")
    badge = "STABLE" if live_stable else "BETA"
    out = layout
    for key, value in {
        "title": html.escape(title),
        "description": html.escape(description),
        "root": root,
        "nav": nav_html(current, root),
        "header_version": html.escape(version),
        "header_badge": badge,
        "repo": html.escape(releases["repo"]),
        "content": content,
    }.items():
        out = out.replace("{{%s}}" % key, value)
    left = re.findall(r"\{\{[a-z_]+\}\}", out)
    if left:
        raise ValueError("%s: unfilled %s" % (title, ", ".join(sorted(set(left)))))
    return out


def build(out_dir):
    releases = json.loads((SITE / "releases.json").read_text(encoding="utf-8"))
    repo = releases["repo"]
    layout = (SITE / "templates" / "layout.html").read_text(encoding="utf-8")
    out_dir.mkdir(parents=True, exist_ok=True)

    # index
    index = (SITE / "templates" / "index.html").read_text(encoding="utf-8")
    title, detail = cta(releases)
    beta = releases.get("beta") or {}
    for key, value in {
        "cta_title": title,
        "cta_detail": detail,
        "maps_checked": html.escape(str(releases.get("maps_checked", ""))),
        "checks": html.escape(str(releases.get("checks", ""))),
        "latest_file": html.escape(beta.get("file", "xerces-<version>.zip")),
        "channel_cards": "\n".join(channel_card(c, releases.get(c), repo) for c in ("stable", "beta")),
        "repo": html.escape(repo),
    }.items():
        index = index.replace("{{%s}}" % key, value)
    pages = {
        "index.html": page(layout, "Xerces: every Black Ops III mod at once",
                           "A free, open-source launcher for Call of Duty: Black Ops III that loads your Steam "
                           "Workshop mods together, in your order.", "index.html", index, releases),
        "download.html": page(layout, "Download Xerces", "Get Xerces and check the download.",
                              "download.html", download_body(releases, repo), releases),
    }
    _, guide, guide_heads = markdown_page(ROOT / "docs" / "TESTERS.md")
    pages["guide.html"] = page(layout, "Guide - Xerces", "How to install and play with Xerces, and what it does.",
                               "guide.html", doc_body("Guide", "Read this first.",
                                                      "Install, play, what to do when the game crashes, and "
                                                      "what Xerces sends and keeps.", guide, guide_heads),
                               releases)
    _, issues, issue_heads = markdown_page(ROOT / "docs" / "KNOWN_ISSUES.md")
    pages["known-issues.html"] = page(layout, "Known issues - Xerces", "What we already know about.",
                                      "known-issues.html", doc_body("Known issues", "Known issues.",
                                                                    "What we already know about, and what to do.",
                                                                    issues, issue_heads), releases)
    changes, change_heads = changelog_body(ROOT / "CHANGELOG.md")
    pages["changelog.html"] = page(layout, "Changelog - Xerces", "Every Xerces release and what changed.",
                                   "changelog.html", doc_body("Changelog", "What changed.", "", changes,
                                                              change_heads), releases)
    for name, text in pages.items():
        (out_dir / name).write_text(text, encoding="utf-8", newline="\n")

    assets = out_dir / "assets"
    if assets.exists():
        shutil.rmtree(assets)
    shutil.copytree(SITE / "assets", assets)
    fonts = assets / "fonts"
    fonts.mkdir(exist_ok=True)
    for f in ("InterTight-wght.ttf", "IBMPlexMono-Regular.ttf", "IBMPlexMono-Medium.ttf",
              "OFL-InterTight.txt", "OFL-IBMPlexMono.txt"):
        shutil.copy2(ROOT / "data" / "xerces" / "fonts" / f, fonts / f)
    (out_dir / ".nojekyll").write_text("", encoding="utf-8")
    return sorted(pages)


def check(out_dir):
    """Every local link and anchor in the built pages resolves."""
    problems = []
    ids = {}
    for p in out_dir.glob("*.html"):
        ids[p.name] = set(re.findall(r'id="([^"]+)"', p.read_text(encoding="utf-8")))
    for p in out_dir.glob("*.html"):
        for href in re.findall(r'href="([^"]+)"', p.read_text(encoding="utf-8")):
            if href.startswith(("http://", "https://", "mailto:")):
                continue
            target, _, anchor = href.partition("#")
            target = target or p.name
            if not (out_dir / target).exists():
                problems.append("%s: %s does not exist" % (p.name, href))
            elif anchor and target.endswith(".html") and anchor not in ids.get(target, set()):
                problems.append("%s: no #%s in %s" % (p.name, anchor, target))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "site"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            build(Path(tmp))
            problems = check(Path(tmp))
    else:
        out = Path(args.out)
        names = build(out)
        problems = check(out)
        print("wrote %s: %s" % (out, ", ".join(names)))
    for p in problems:
        print("problem:", p)
    if args.check:
        print("site: ok" if not problems else "site: %d problem(s)" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
