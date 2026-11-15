# Xerces' website

The website for players: what Xerces is, the download with its SHA-256, the
guide, the known issues and the changelog. It is plain files (HTML, one
stylesheet, the launcher's fonts), made from this repository so a release
updates it from the same text its zip carries.

## Build

```
python -I tools/site/build_site.py            # writes build/site
python -I tools/site/build_site.py --check    # builds into a temporary folder; exit 1 on a broken link
```

To look at it, serve `build/site` with any static server, for example
`python -m http.server 8794 --bind 127.0.0.1 --directory build/site`.

## Where the pages come from

| Page | Source |
| --- | --- |
| `index.html` | `site/templates/index.html`, with the release data below |
| `download.html` | `tools/site/build_site.py` (`download_body`) and `site/releases.json` |
| `guide.html` | `docs/TESTERS.md` (the zip's README.txt) |
| `known-issues.html` | `docs/KNOWN_ISSUES.md` (the zip's KNOWN_ISSUES.txt) |
| `changelog.html` | `CHANGELOG.md` |

Every page shares `site/templates/layout.html` (header, navigation, footer, the
theme switch). `site/assets/site.css` is the launcher's look, C / Signal
(docs/DESIGN.md 7.2): its colours are the ones in `data/xerces/ui/tokens.json`,
its fonts are the launcher's (Inter Tight and IBM Plex Mono, SIL OFL, copied
from `data/xerces/fonts` with their licences). Nothing is loaded from another
site: no font service, no analytics, no scripts but the menu and the theme
switch.

## A release

1. Build and check the zip as always (`tools\release.ps1`).
2. Put the release in `site/releases.json`: the channel's `version`, `date`,
   `file`, `size_bytes`, `sha256` and, for a public release, `url` (the GitHub
   Release asset); move the old entry into `history`. A channel's `state` is
   `planned` (no release yet: the card says it is being prepared) or `live`.
3. Add the release to `CHANGELOG.md` (an `## [version] - date` section).
4. Build, check, and publish `build/site`.

## Publishing

Not set up yet: where the site lives is the maintainer's call. It is a folder
of static files, so GitHub Pages (from a branch or an Actions workflow that runs
the build above), Cloudflare Pages, Netlify or the backend's host all work. The
build writes `.nojekyll` for GitHub Pages.
