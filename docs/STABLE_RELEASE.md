# The first stable release (1.0.0): the plan

Written 2026-11-15 from where the betas stand (beta.27 and the night after it)
and docs/DESIGN.md section 16. It says what a stable release is, what has to be
true before one goes out, what is done, and what only the maintainer can
decide. The boxes are ticked as things are done.

## What "stable" means here

- A release players can install from a public page, check, and trust to play
  their own mod stack on their own maps: no crash in the common paths, a clear
  message (and a report) when something goes wrong, and nothing on by default
  that has not been played through.
- Betas keep coming, ahead of it: every change goes to a beta first, and a stable
  release is a beta that has been played for a while, with only fixes after
  it.
- Versions: `1.0.0` for the first; fixes `1.0.1` and so on; the betas towards
  the next one are `1.1.0-beta.N`. `VERSION` stays the one place it is set.

## Gates (all before 1.0.0)

### The game

- [x] The M1 cases (launcher to game, nine cases, 60 checks) pass on every build;
      the packaged beta.27 passes them from its own unzipped files.
- [x] The maintainer's own stack (eight mods: DS4C, Ascendance, im_oxo QOL,
      Classic Viewmodel FOV, DBZ Villains, Girls' Frontline 2.0, Avali,
      Neptunia) with their own settings on every installed Zombies map: the
      sweep of 2026-11-15 (docs/TESTING.md).
- [x] No mod's client systems left out (the shared slot, 68f788cc).
- [ ] The two-PC test (docs/TWO_PC_TEST.md) with a friend: hosting, joining,
      a missing mod, map changes, faster loads with someone in the lobby. Needs
      a second person; nothing else can stand in for it.
- [ ] A week of the release candidate in testers' hands with no new crash in
      the reports (the crash backend shows them).
- [ ] KNOWN_ISSUES.md gone over: each entry either fixed, or written for
      players with what to do.

### The package

- [x] `tools\release.ps1`: lints, unit tests, the CLI smoke test, the build, the
      notices and the zip with SHA256SUMS; the SHA-256 of the zip goes out apart
      from it.
- [x] SOURCE.txt names the exact commit and every submodule.
- [ ] A source archive with the submodules attached to each release (GitHub's
      own archives leave them out; GPL, DESIGN.md 16.1).
- [ ] NOTICE and CREDITS, and an About and Legal page (DESIGN.md 16.5).
- [ ] README.txt for players, not testers: docs/TESTERS.md speaks of a private
      link and "this beta"; a stable zip needs a version of it without that.
- [ ] Code signing for Xerces.exe, xerces_cli.exe and boiii.exe (DESIGN.md
      16.4: "v1.0 is published publicly only after this is in place"). Needs a
      certificate or a signing service: the maintainer's decision and cost.

### Getting it to players

- [x] A website made from the repository (site/, tools/site/build_site.py):
      landing page, download page with the SHA-256, guide, known issues,
      changelog, phone layout, no third-party requests.
- [x] CHANGELOG.md in the Keep a Changelog way; the website's changelog page is
      made from it.
- [ ] Where the website lives (GitHub Pages, Cloudflare Pages, the backend's
      host): the maintainer's decision. Then a workflow that builds it.
- [ ] A GitHub Release `v1.0.0` with the zip, SHA256SUMS.txt and the source
      archive, its notes from CHANGELOG.md. Tags and releases are the
      maintainer's to push.
- [ ] Updates (DESIGN.md 16.2: stable and beta channels, signed manifests,
      staged swap with rollback) are not built. 1.0.0 can go out without them
      (players replace the folder, as with the betas); the launcher then needs
      at least a "a newer version is out" notice, or the website says to check.

## What only the maintainer can decide

1. **Signing**: buy a code-signing certificate (or a signing service) for 1.0.0,
   or publish 1.0.0 unsigned with the SHA-256 and the SmartScreen steps (the
   betas' way), against DESIGN.md 16.4.
2. **Updates**: ship 1.0.0 without in-app updates, or build the update channel
   first (several days of work and a release key kept offline).
3. **Where the website lives**, and its address.
4. **Public or invite-only**: a public GitHub Release and website, or the
   private-link way the betas went out.
5. **When**: after the two-PC test and a week of a release candidate, at the
   earliest.

## How a release is cut (once the gates are ticked)

1. Freeze: only fixes on the release branch.
2. `VERSION` to `1.0.0`, the `## [1.0.0] - <date>` section in CHANGELOG.md, the
   DESIGN.md version line, `site/releases.json` (stable: `live`).
3. `tools\release.ps1` on a clean tree that is on origin; the M1 cases on the
   package's own files; the zip's SHA-256 written down.
4. The maintainer tags `v1.0.0`, makes the GitHub Release with the zip,
   SHA256SUMS.txt and the source archive, and publishes the website.
