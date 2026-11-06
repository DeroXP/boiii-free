# Xerces

Xerces is a multi-mod launcher and loader for Call of Duty: Black Ops III, for
people who own the game on Steam. It loads several Steam Workshop mods at once,
in the order you choose, says before launch which mods clash, and helps when a
mix of mods breaks the game. It plays offline and on games players host
themselves.

Xerces is a modified version of
[Ezz BOIII](https://github.com/Ezz-lol/boiii-free), itself based on BOIII by
momo5502 and its contributors. It keeps their code, credits and licence.

## Status

Beta. Testers get a zip by private link; there is no public release yet. What
works, what was tested and how is in [docs/TESTING.md](docs/TESTING.md); what
testers should know is in [docs/TESTERS.md](docs/TESTERS.md) (README.txt in the
zip) and [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

## What Xerces does

- **Several mods at once.** Workshop mods load together, with the top one as the
  main mod, through the mod-stack loader taken from Ezz BOIII.
- **Conflicts before launch.** Xerces reads what every mod changes and says
  which ones clash, and which custom maps will not fit in the game's memory with
  the mods you picked.
- **Crash recovery.** After a crash with mods on, Xerces offers to start without
  them, and "Find the culprit" narrows a crash down to one mod.
- **Packs.** A mod list with its order and settings, shared as a file or a code.
- **Playing together.** A host opens its lobby to direct connect (the game asks
  the host's router to forward its port with UPnP), others join by address, and
  the joiner's game loads the host's mods or names the ones it lacks, with their
  Workshop pages. A host can ask for a password that its own game checks. Not
  yet tested on two PCs ([docs/TWO_PC_TEST.md](docs/TWO_PC_TEST.md)).
- **Crash reports.** On by default, off in Settings > Privacy: a redacted report
  with a stacks-only dump goes to the Xerces server
  ([docs/CRASH_REPORTS.md](docs/CRASH_REPORTS.md)).
- **Diagnostics.** The game build, the mods, logs and crash bundles in one
  place, and a report to paste into a bug report.
- **Game builds checked.** Xerces starts only Black Ops III builds it knows, and
  finds the engine functions it needs by pattern, so a Steam update stops it
  cleanly instead of crashing.

## What Xerces does not do

- Official online matchmaking. Xerces is for offline play and self-hosted games
  only.
- Getting mods around Steam. Mods come from your own Workshop subscriptions,
  through the Steam client. Xerces never downloads Workshop content itself and
  never passes it between players.
- Playing without owning the game. Xerces is for people who own Black Ops III on
  Steam.

## Requirements

- Windows 10 or 11, 64-bit.
- Call of Duty: Black Ops III on Steam, at a supported build
  ([compat/builds.json](compat/builds.json)), and Steam running.

## Using it

Unzip the package into a folder of its own and start `Xerces.exe`. It starts the
game client (`boiii.exe`) itself. The rest is in
[docs/TESTERS.md](docs/TESTERS.md).

## Reporting bugs

Open an issue at <https://github.com/DeroXP/boiii-free/issues>. Please include
the Xerces version (About, or the top of the game's main menu), what you did and
what happened, the mods you had on in their order, the mode and map, and
Diagnostics > Copy report. Never attach game files.

## Building

Requirements: LLVM 23 (clang and LLD), Ninja, premake 5.0.0, and the Visual
Studio 2022 Build Tools with the C++ workload and the "C++ ATL for latest v143
build tools (x86 & x64)" component. The launcher also needs RmlUi 6.3 and
FreeType 2.14.3 sources in one folder (`rmlui\RmlUi-6.3`,
`freetype\freetype-2.14.3`), and shapes its text with HarfBuzz 15.0.0 when the
folder also has `harfbuzz\harfbuzz-15.0.0`; their archives and hashes are in
[docs/TESTING.md](docs/TESTING.md).

```
git clone --recursive -b xerces-main https://github.com/DeroXP/boiii-free.git xerces
cd xerces
tools\build.ps1 -Tests                                 # core, unit tests, CLI checks
tools\build.ps1 -Client                                # the game client, boiii.exe
tools\build.ps1 -Launcher -ThirdPartyDir <folder>      # the launcher, Xerces.exe
```

Xerces is the `xerces-main` branch; the repository's default branch is the
fork's earlier multi-mod proof of concept. To rebuild a package, check out the
commit its `SOURCE.txt` names instead (then
`git submodule update --init --recursive`).

`-ToolsDir <folder>` finds the tools when they are not on `PATH`. The programs
land in `build\bin\x64\Release`. Run `Xerces.exe` from there: it finds `data\`
and `compat\` in the checkout.

A tester package comes from `tools\release.ps1` on a clean, committed tree: it
builds and tests everything, then writes `dist\xerces-<version>.zip` (the
version is in [VERSION](VERSION)), with `THIRD_PARTY_NOTICES.txt` and
`SOURCE.txt` made by `tools\release\notices.py`, and the symbols separately.

Design, decisions and research notes are in [docs/DESIGN.md](docs/DESIGN.md) and
[docs/research](docs/research). [docs/FENCED.md](docs/FENCED.md) lists inherited
code Xerces never changes, and [docs/ENDPOINTS.md](docs/ENDPOINTS.md) every host
the programs can contact. GSC script additions are in
[docs/gsc-scripting.md](docs/gsc-scripting.md).

## Credits

- **BOIII** by momo5502 and its contributors: the client Xerces starts from.
- **Ezz BOIII** by Ezz and its contributors, including the mod-stack branch
  Xerces loads several mods with.
- From upstream's credits: Likeicareaboutit (the Steam Workshop downloader,
  which Xerces builds leave out) and framilano (the BO3 server installer).
- The libraries Xerces is built with are listed, with their licences, in the
  `THIRD_PARTY_NOTICES.txt` of every package.

## Licence

Xerces is free software under the GNU General Public License, version 3
([LICENSE](LICENSE)), with no warranty. Each package's `SOURCE.txt` names the
exact commit it was built from.

Call of Duty and Black Ops III are trademarks of Activision. Xerces is not made,
endorsed or supported by Activision or Treyarch, and adds no game data: you play
with your own copy of the game from Steam.
