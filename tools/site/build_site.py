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
sys.path.insert(0, str(ROOT / "tools" / "release"))
import flavour  # noqa: E402  (the guide's beta and stable lines, as in README.txt)



# ------------------------------------------------------------------ markdown
def slug(text):
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text).lower()
    text = re.sub(r"[`*_]", "", text)
    text = re.sub(r"[^a-z0-9]+", "-", text).strip("-")
    return text or "section"


# Links between the docs (written for GitHub) to the site's own pages
MD_PAGES = {
    "TESTERS.md": "guide.html",
    "KNOWN_ISSUES.md": "known-issues.html",
    "mod-stack.md": "mod-stacking.html",
    "gsc-scripting.md": "gsc-scripting.html",
    "asset-override.md": "asset-overrides.html",
    "fastdl.md": "fastdl.html",
    "CHANGELOG.md": "changelog.html",
}


def site_href(href):
    target, sep, anchor = href.partition("#")
    name = target.rsplit("/", 1)[-1]
    if target and name in MD_PAGES:
        return MD_PAGES[name] + sep + anchor
    return href


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
                   lambda m: '<a href="%s">%s</a>' % (html.escape(site_href(m.group(2))), m.group(1)), s)
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
# The bar: (href, label, kind); kind "ext" opens another site, "get" is the button
def nav_items(repo):
    return [
        ("docs.html", "Docs", ""),
        ("changelog.html", "Changelog", ""),
        ("tested.html", "Tested", ""),
        (repo, "GitHub", "ext"),
        ("download.html", "Download", "get"),
    ]


# The docs that are pages of their own: (page, source, title, eyebrow, one line, heading shift)
DOCS_PLAYERS = [
    ("guide.html", "docs/TESTERS.md", "Read this first.", "Guide",
     "Install, play, what to do when the game crashes, and what Xerces sends and keeps.", 0),
    ("known-issues.html", "docs/KNOWN_ISSUES.md", "Known issues.", "Known issues",
     "What we already know about, and what to do.", 0),
]
DOCS_AUTHORS = [
    ("mod-stacking.html", "docs/mod-stack.md", "Mod stacking.", "Mod authors",
     "How several Workshop mods load at once, which one wins, and how servers set a stack.", 0),
    ("gsc-scripting.html", "docs/gsc-scripting.md", "GSC scripting additions.", "Mod authors",
     "Where scripts can live, hooks, console commands, file and data helpers, player helpers.", 1),
    ("asset-overrides.html", "docs/asset-override.md", "Asset overrides.", "Mod authors",
     "Replacing the game's assets with loose files, and which kinds it supports.", 0),
    ("fastdl.html", "docs/fastdl.md", "FastDL.", "Server hosts",
     "Serving a server's mods over HTTP so joining players download them quickly.", 0),
]


def nav_html(current, root, repo):
    out = []
    for href, label, kind in nav_items(repo):
        external = kind == "ext"
        target = href if external else root + href
        attrs = ' class="ext" rel="noopener"' if external else (' class="get"' if kind == "get" else "")
        cur = ' aria-current="page"' if href == current and kind != "get" else ""
        out.append('      <a href="%s"%s%s>%s</a>' % (html.escape(target), attrs, cur, label))
    return "\n".join(out)


def human_size(n):
    return "%.1f MB" % (n / 1_000_000)


def latest_label(releases):
    stable = releases.get("stable") or {}
    beta = releases.get("beta") or {}
    if stable.get("state") == "live":
        return "Version %s" % html.escape(stable["version"])
    if beta:
        return "Beta %s" % html.escape(beta["version"])
    return "First release in preparation"


def channel_card(name, data):
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
        <a class="button" href="changelog.html">What it will have</a>
      </article>""" % (primary, title, html.escape(data.get("summary", "")), html.escape(data["version"]))
    rows = [("Version", html.escape(data["version"]), ""), ("Released", html.escape(data.get("date", "")), "")]
    if data.get("size_bytes"):
        rows.append(("Size", human_size(data["size_bytes"]) + " zip", ""))
    if data.get("sha256"):
        rows.append(("SHA-256", html.escape(data["sha256"]), ' class="hash"'))
    dl = "".join("<dt>%s</dt><dd%s>%s</dd>" % (k, cls, v) for k, v, cls in rows)
    if data.get("url"):
        action = '<a class="button primary" href="%s">Download %s <span class="meta">%s</span></a>' % (
            html.escape(data["url"]), html.escape(data["version"]), html.escape(data.get("file", "")))
    else:
        action = "<p>%s</p>" % html.escape(data.get("access", ""))
    return """      <article class="channel%s">
        <div class="top"><h3>%s</h3><span class="state live">Current</span></div>
        <dl>%s</dl>
        %s
      </article>""" % (primary, title, dl, action)


def head(eyebrow, title, lede=""):
    return """<section class="page-head"><div class="wrap">
  <div class="eyebrow">%s</div>
  <h1>%s</h1>
  %s
</div></section>""" % (eyebrow, html.escape(title), ("<p>%s</p>" % lede) if lede else "")


def download_body(releases, repo):
    cards = "\n".join(channel_card(c, releases.get(c)) for c in ("stable", "beta"))
    hist = []
    for h in releases.get("history", []):
        sha = '<code>%s</code>' % html.escape(h["sha256"]) if h.get("sha256") else "&mdash;"
        hist.append("<tr><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            html.escape(h["version"]), html.escape(h.get("date", "")), sha))
    zip_name = html.escape((releases.get("beta") or {}).get("file", "xerces-<version>.zip"))
    return head("Download", "Get Xerces.",
                "One zip, no installer. Check it, unzip it into a folder of its own, start Xerces.exe.") + """
<section class="band"><div class="wrap">
  <div class="channels">
%s
  </div>
  <p class="notice"><strong>Not code-signed yet.</strong> Windows SmartScreen may say it protected your PC. Check the zip's SHA-256 first; if it matches, choose "More info", then "Run anyway".</p>
</div></section>
<div class="wrap doc">
  <nav class="toc" aria-label="On this page"><a href="#install">Install</a><a href="#verify">Check a download</a><a href="#need">What you need</a><a href="#updating">Updating</a><a href="#earlier">Earlier releases</a><a href="#source">Build it yourself</a></nav>
  <article class="prose">
  <h2 id="install">Install</h2>
  <ol class="steps">
    <li><div><h3>Own Black Ops III on Steam</h3><p>Installed, with Steam running and signed in. Your mods come from your Workshop subscriptions.</p></div></li>
    <li><div><h3>Check the zip</h3><p>Compare its SHA-256 with the one on this page (below).</p></div></li>
    <li><div><h3>Unzip it into a folder of its own</h3><p>For example <code>C:\\Games\\Xerces</code>. Not inside the Black Ops III folder, and not in a folder OneDrive syncs, where the game can fail to start.</p></div></li>
    <li><div><h3>Start Xerces.exe</h3><p>Only Xerces.exe: it starts the game itself, with your mods. The first start reads your Workshop folder once; later starts are quick. Your settings live in <code>%%LOCALAPPDATA%%\\Xerces</code>, so a new version is a new folder.</p></div></li>
  </ol>
  <h2 id="verify">Check a download</h2>
  <p>In PowerShell, in the folder with the zip, run this with your zip's name and compare what it prints with the SHA-256 on this page. If they differ, do not use the zip, and tell us.</p>
  <pre><code>Get-FileHash .\\%s</code></pre>
  <p><code>SHA256SUMS.txt</code> inside the zip lists every file's hash, to check later that nothing in the folder changed. It came in the same zip, so only the zip's own hash shows the zip is the one we made.</p>
  <h2 id="need">What you need</h2>
  <ul>
    <li>Windows 10 or 11, 64-bit.</li>
    <li>Black Ops III on Steam, installed, at a build Xerces supports (Diagnostics says which).</li>
    <li>Steam running and signed in with the account that owns the game.</li>
    <li>Nothing else: no installer, no runtimes, no admin rights.</li>
  </ul>
  <h2 id="updating">Updating</h2>
  <p>Xerces does not update itself yet: a new version is a new zip on this page (programs can read <a href="releases.json">releases.json</a>). Close Xerces and the game, delete the files in your Xerces folder, and unzip the new zip there. Your settings stay in <code>%%LOCALAPPDATA%%\\Xerces</code>.</p>
  <p>After Steam updates Black Ops III, Xerces launches the game again once a version that knows the new game build is out (until then, play through Steam); this page has it when it is.</p>
  <h2 id="earlier">Earlier releases</h2>
  <table><thead><tr><th>Version</th><th>Date</th><th>SHA-256</th></tr></thead><tbody>%s</tbody></table>
  <p>The list for programs is <a href="releases.json">releases.json</a>.</p>
  <h2 id="source">Build it yourself</h2>
  <p>Every release names the commit it was built from in its SOURCE.txt, and comes with <code>xerces-&lt;version&gt;-source.zip</code>: that commit with the libraries it is built from (GitHub's own archives leave them out). The <a href="%s">source</a> builds with the pinned tools in its README, and the result can be compared file by file.</p>
  </article>
</div>""" % (cards, zip_name, "".join(hist), html.escape(repo))


def doc_body(eyebrow, title, lede, body, headings):
    toc = "\n".join('<a href="#%s">%s</a>' % (hid, text) for level, hid, text in headings if level == 2)
    return head(eyebrow, title, lede) + """
<div class="wrap doc">
  <nav class="toc" aria-label="On this page">%s</nav>
  <article class="prose">
%s
  </article>
</div>""" % (toc, body)


def docs_index_body():
    def group(title, docs):
        items = "".join('<a href="%s"><h3>%s</h3><p>%s</p></a>' % (p, html.escape(t.rstrip(".")), html.escape(line))
                        for p, _, t, _, line, _ in docs)
        return '<div class="doc-group"><div class="eyebrow">%s</div><div class="doc-list">%s</div></div>' % (
            title, items)
    players = DOCS_PLAYERS + [("download.html#install", "", "Install", "", "Four steps, and how to check a download.", 0),
                              ("changelog.html", "", "Changelog", "", "Every release and what changed.", 0)]
    return head("Documentation", "Docs.", "For players, mod authors and server hosts.") + """
<section class="band"><div class="wrap">
%s
%s
</div></section>""" % (group("Players", players), group("Mod authors and server hosts", DOCS_AUTHORS))


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


def tested_body(tested):
    """site/tested.json: a mod stack on every map, as the release was checked."""
    rows = []
    ok = 0
    for m in tested["maps"]:
        good = m["result"] == "in_game"
        ok += good
        rows.append("<tr><td>%s</td><td><code>%s</code></td><td>%s</td><td>%s</td></tr>" % (
            html.escape(m["name"]), html.escape(m["map"]),
            "Played" if good else "<strong>%s</strong>" % html.escape(m.get("note", m["result"])),
            ("%.1f s" % m["load_s"]) if good and m.get("load_s") else "&mdash;"))
    mods = "".join("<li>%s</li>" % html.escape(n) for n in tested["stack"])
    return head("Tested", "Every map, eight mods.",
                "Before a release, a real mod stack goes onto every installed Zombies map: one game per map, "
                "through Xerces.exe, loaded and played. %d of %d played on %s." % (
                    ok, len(tested["maps"]), html.escape(tested["build"]))) + """
<div class="wrap doc">
  <nav class="toc" aria-label="On this page"><a href="#stack">The stack</a><a href="#maps">The maps</a><a href="#how">How</a></nav>
  <article class="prose">
  <h2 id="stack">The stack</h2>
  <p>In this load order (the first is the main mod), with %s.</p>
  <ol>%s</ol>
  <h2 id="maps">The maps</h2>
  <table><thead><tr><th>Map</th><th>Name in the game</th><th>Result</th><th>Load</th></tr></thead><tbody>%s</tbody></table>
  <p>Load: from the console's <code>map</code> command to the player in the level, with Xerces' faster loads.</p>
  <h2 id="how">How</h2>
  <p>%s</p>
  </article>
</div>""" % (html.escape(tested["settings"]), mods, "".join(rows), html.escape(tested["how"]))


def page(layout, title, description, current, content, releases, root=""):
    beta = releases.get("beta") or {}
    stable = releases.get("stable") or {}
    live_stable = stable.get("state") == "live"
    version = ("Version %s" % stable["version"]) if live_stable else ("Beta %s" % beta.get("version", ""))
    out = layout
    for key, value in {
        "title": html.escape(title),
        "description": html.escape(description),
        "root": root,
        "nav": nav_html(current, root, releases["repo"]),
        "header_version": html.escape(version),
        "repo": html.escape(releases["repo"]),
        "content": content,
    }.items():
        out = out.replace("{{%s}}" % key, value)
    left = re.findall(r"\{\{[a-z_]+\}\}", out)
    if left:
        raise ValueError("%s: unfilled %s" % (title, ", ".join(sorted(set(left)))))
    return out


def feed(releases):
    """releases.json at the site's root: what a program (the launcher's update notice, a script) reads."""
    out = {"schema": "xerces.releases/1", "repo": releases["repo"], "channels": {}}
    for name in ("stable", "beta"):
        data = releases.get(name)
        if not data or data.get("state") != "live":
            continue
        out["channels"][name] = {k: data[k] for k in ("version", "date", "file", "size_bytes", "sha256", "url")
                                 if data.get(k)}
        out["channels"][name]["notes"] = "changelog.html#" + slug(data["version"])
    return json.dumps(out, indent=2) + "\n"


def build(out_dir):
    releases = json.loads((SITE / "releases.json").read_text(encoding="utf-8"))
    repo = releases["repo"]
    layout = (SITE / "templates" / "layout.html").read_text(encoding="utf-8")
    out_dir.mkdir(parents=True, exist_ok=True)

    index = (SITE / "templates" / "index.html").read_text(encoding="utf-8")
    index = index.replace("{{latest_label}}", latest_label(releases)).replace("{{repo}}", html.escape(repo))
    pages = {
        "index.html": page(layout, "Xerces: Black Ops III, stacked",
                           "A free, open-source launcher for Call of Duty: Black Ops III that loads your Steam "
                           "Workshop mods together, in your order.", "index.html", index, releases),
        "download.html": page(layout, "Download - Xerces", "Get Xerces and check the download.",
                              "download.html", download_body(releases, repo), releases),
        "docs.html": page(layout, "Docs - Xerces", "Xerces' documentation for players, mod authors and server "
                          "hosts.", "docs.html", docs_index_body(), releases),
    }
    # The guide says what README.txt in the zip on the download page says: the
    # stable wording once a stable release is live.
    stable_live = (releases.get("stable") or {}).get("state") == "live"
    for name, source, title, eyebrow, line, shift in DOCS_PLAYERS + DOCS_AUTHORS:
        text = flavour.pick((ROOT / source).read_text(encoding="utf-8"), stable_live).split("\n")
        if text and text[0].startswith("# "):
            text = text[1:]
        headings = []
        body = render_blocks(text, headings, shift)
        pages[name] = page(layout, "%s - Xerces" % title.rstrip("."), line, "docs.html",
                           doc_body(eyebrow, title, line, body, headings), releases)
    changes, change_heads = changelog_body(ROOT / "CHANGELOG.md")
    pages["changelog.html"] = page(layout, "Changelog - Xerces", "Every Xerces release and what changed.",
                                   "changelog.html", doc_body("Changelog", "What changed.", "", changes,
                                                              change_heads), releases)
    tested_path = SITE / "tested.json"
    if tested_path.exists():
        tested = json.loads(tested_path.read_text(encoding="utf-8"))
        pages["tested.html"] = page(layout, "Tested - Xerces", "A real mod stack on every map, before a release.",
                                    "tested.html", tested_body(tested), releases)
    for name, text in pages.items():
        (out_dir / name).write_text(text, encoding="utf-8", newline="\n")
    (out_dir / "releases.json").write_text(feed(releases), encoding="utf-8", newline="\n")

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
