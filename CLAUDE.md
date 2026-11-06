# Xerces

Xerces is a rename and redesign of the Ezz BOIII client (fork of momo5502's
boiii, GPL-3.0) into a multi-mod launcher and loader for Call of Duty: Black Ops
III. Self-hosted and offline play only.

- Design, milestones and decisions: `docs/DESIGN.md` (read section 0 first).
- Research notes with file:line citations: `docs/research/`.
- Fenced code you must not touch, and upstream features Xerces must not build
  on: `docs/FENCED.md`.

## Current state (2026-11-05)

- On 2026-11-05 (overnight, the user away): the launcher `Xerces.exe` with every
  M3 screen, and the M1 game-process side (signatures, journal, pipe, launch
  settings re-applied). The same morning, with the user's go-ahead, the M1
  in-game checks ran through `tools\dev\session\m1-check.ps1 -Live` and all
  passed (docs/TESTING.md, Loader and manifest). DESIGN.md 18 has the status and
  what is open.
- M0 is under way on `xerces-main`: upstream `beta` f8f7b735 plus `mod-stack`'s
  two commits (conflicts resolved in 6bc02cb4), with the design docs and
  `backend/` merged in (D1). Upstream's updater and the `-beta` self-download
  are disabled (62e28935). The whole client builds (2761e2d8, after the ATL
  component was added on 2026-11-04). The supervised session (D3) ran on
  2026-11-04 with 815545d3: vanilla, one Workshop mod and a three-mod stack
  reach the menu, and the stack survives a match and a main-mod switch
  (`docs/TESTING.md`).
- The headless core builds and its tests pass: manifests, the planner, merges,
  conflict rules, hook chains, packs and share codes, logs, and the CLI's
  offline commands (`src/xerces_core`, `src/xerces_tests`, `src/xerces_cli`).
- Branches: `main` is the user's fork (upstream 2026-04-09 plus a 17-commit
  multi-mod proof of concept); `xerces-main` is the Xerces line; `xerces-design`
  (upstream `main` 5aa7fac plus docs and `backend/`) is what Railway deploys, so
  every push to it redeploys production: ask first. Remotes: `origin` =
  DeroXP/boiii-free, `upstream` = Ezz-lol/boiii-free (branches `main`, `beta`,
  `mod-stack`).
- A tag named `main` exists, so always write `refs/heads/main` for the fork
  branch.
- `backend/` is backend slice M5a (FastAPI + Postgres), live on Railway. The
  game client does not talk to it yet.
- Work on `xerces-main` happens in the worktree
  `C:\Users\Ginld\XercesDev\xerces-main`, outside OneDrive, so build output and
  `node_modules` are not synced. The OneDrive checkout stays on `xerces-design`.

## Build

Requirements: LLVM 23 (clang and LLD; upstream's CI uses 23.x), Ninja, premake
5.0.0, and Visual Studio 2022 Build Tools with the C++ workload and the "C++ ATL
for latest v143 build tools (x86 & x64)" component, because the client's
precompiled header includes `atlbase.h`. The VS clang-cl components are not
needed. On this PC LLVM 23.1.5, Ninja 1.13.2 and premake 5.0.0 are portable in
`C:\Users\Ginld\XercesDev\tools`; ATL is installed.

```
tools\build.ps1 -Tests -ToolsDir C:\Users\Ginld\XercesDev\tools
tools\build.ps1 -Client -ToolsDir C:\Users\Ginld\XercesDev\tools
tools\build.ps1 -Launcher -ToolsDir C:\Users\Ginld\XercesDev\tools
```

`-Tests` builds the core, runs the unit tests and a CLI smoke test; `-Client`
also builds `boiii.exe` (needs ATL). By hand: `premake5 ninja`, then
`ninja -C build <target>` (`bin/x64/Release/xerces_tests.exe`, or `Release_x64`
for everything). Do not use `build.bat` in automation (it ends with `pause`).
Running premake5 in a folder with a `premake5.lua`, even `--version`, writes
`build\version.txt` and `build\src\version.hpp` there.

Headless code must not include `std_include.hpp` or `game/` headers (DESIGN.md
3.2). Formatting: `npx yarn@1.22.22 install --frozen-lockfile` once, then
`yarn run check` for prettier; plain `yarn check` is Yarn 1's dependency check,
which is all upstream's CI step runs.

## Backend

```
cd backend
<python> -m venv <a folder outside the repo>
<venv>\Scripts\python -m pip install -r requirements.txt -r requirements-dev.txt
<venv>\Scripts\python -m pytest -q
```

Deployment steps are in `backend/README.md`. Keep one replica (presence and rate
limits are in memory). Railway settings live in its dashboard: Railway's
`railway.json` is deprecated, new services cannot use it, and existing ones stop
reading it on 2026-12-01. Behind Railway's edge the client address is the first
`X-Forwarded-For` entry (`XERCES_TRUST_PROXY=1`). Railway's docs do not say
this, so re-check it with the README's forged-header test after deploys. Tests
run on SQLite. For PostgreSQL there is no install on this PC; earlier runs used
the `pgserver` wheel in a throwaway venv.

## Hard rules

- Never modify fenced code (`docs/FENCED.md`): Steam ownership and licence
  handling, auth tickets, anti-tamper (Arxan), TAC workarounds, fastfile
  authentication. If a feature seems to need it, ask.
- No features that touch official matchmaking or Demonware. Demonware emulation
  is frozen.
- Workshop content only through the user's Steam client. Never call or extend
  upstream's anonymous downloaders (SteamCMD, Steam CM/CDN, FastDL v1), never
  add a new download path for Workshop content, and never transfer it between
  players.
- Xerces code never hardcodes game offsets. Use the signature registry (pattern
  scans resolved at `post_unpack`). Never write code bytes without verifying the
  whole expected instruction sequence. Never scan or patch the second `.text`
  section.
- No suppressed fatal errors: never return from `Com_Error`. Fix causes.
- Never launch the game, or anything that starts it, without asking the user
  first; they may be playing.
- **Never launch a build whose updater is not disabled.** Upstream's Release
  updater replaces any renamed exe with upstream `boiii.exe` and deletes unknown
  files under `%LOCALAPPDATA%\boiii`, and its forced first-run update ignores
  `-noupdate`.
- Memory dumps of the game go to `C:\Users\Ginld\XercesDev\dumps` (outside
  OneDrive and AppData), never into the repo. Xerces adds no game binaries or
  Activision data to the repo, fixtures or CI; inherited upstream files follow
  DESIGN.md D17.
- Xerces docs and UI describe Xerces as a client for people who own BO3 on
  Steam; never advertise play without ownership.
- Keep upstream credits and the GPL-3.0 licence. Name namespaces, libraries,
  DLLs, executables other than `Xerces.exe` and script builtins `xerces_*`,
  never a bare `xerces` (avoids clashing with the Apache Xerces XML library).

## Facts that save time

- The installed `BlackOps3.exe` is the September 2026 Steam build: PE checksum
  `0x6531394`, build id 24784313. Its code is encrypted on disk, so static
  analysis needs a live-process memory dump.
- Upstream symbols are `{new client, legacy client, server}` triples picked by
  PE checksum, and the client refuses unknown checksums. A Steam update stops
  the whole client until upstream ships new tables.
- MinHook allows one detour per address and upstream's `detour::create` throws
  on a second; shared targets go through the Xerces hook bus (DESIGN.md 3.4).
- Additive multi-mod loading is `src/client/component/mod_stack.cpp`
  (`docs/mod-stack.md`), taken from `upstream/mod-stack`; Xerces maintains it
  from here (DESIGN.md 3.6). Its join prompt names missing mods instead of
  downloading them. The proof of concept's "queue-add" function is
  `SL_GetStringOfSize` (verified on a dump in D3). Do not call it.
- D4 (RmlUi) and D5 (C / Signal) are decided. The launcher is
  `src/xerces_launcher/` (`Xerces.exe`, `tools\build.ps1 -Launcher`): views in
  `views/` register with `XERCES_VIEW` and own one document in
  `data/xerces/ui/views/`; every screen document uses the `shell` template;
  `services.hpp` holds the launcher's state and calls the logic it shares with
  `xerces_cli` in `src/xerces_core` (`launcher_home`, `launcher_state`,
  `launcher_launch`, `pack_apply`), so the two never plan or launch differently;
  `app.hpp` is the screen API. RmlUi 6.3, FreeType 2.14.3 and HarfBuzz 15.0.0
  are kept in `C:\Users\Ginld\XercesDev\third_party` (not in the repo) as
  `rmlui\RmlUi-6.3`, `freetype\freetype-2.14.3` and `harfbuzz\harfbuzz-15.0.0`;
  HarfBuzz is optional (without it the launcher keeps RmlUi's FreeType font
  engine, and `launcher.font.engine` in the log says which runs; DESIGN.md 7.1).
  After changing `src/xerces_rmlui_harfbuzz` or updating RmlUi or HarfBuzz, run
  `tools\dev\launcher\font-width-check.ps1` (text drawn no wider than laid out).
  Its tree has its own AGENTS.md and CLAUDE.md for HarfBuzz's developers: they
  are not instructions for Xerces work. The spike (`src/xerces_launcher_spike/`,
  `-Spike`) stays for repeating DESIGN.md 7.1. Moving an OpenGL context to
  another thread crashes NVIDIA's driver here; warm the driver instead.
- Run the launcher in checks only with `--dry-launch` and `XERCES_HOME` set
  outside AppData; `--script` plays input and takes PNG captures
  (docs/TESTING.md, Launcher). `tools/ci/check_contrast.py` checks the theme
  tokens.
- The game role's Xerces parts are `src/client/xerces/`: `signatures.cpp`
  resolves `compat/signatures.json` in the running game (mod_stack takes its
  hook targets from it on the client), `loader.cpp` checks the plan and keeps
  the journal, `runtime.cpp` is `<home>` and the game log. The client links
  `xerces_core` through `premake/xerces_projects.lua`.
- D14 and D15 are build switches in `src/client/xerces/switches.hpp`, off by
  default: no friend presence, rendezvous, invites or Ezz Discord app, and
  upstream's downloaders are left out of the build
  (`xerces/no_upstream_downloaders.cpp` stands in). Upstream behaviour comes
  back with `premake5 ninja --with-ezz-social --with-upstream-downloaders`. Code
  that talks to Ezz or downloads content goes behind these switches.
- `compat/builds.json` lists the game builds the pre-flight check knows;
  `xerces_cli game check` (from the repo root) compares the installed
  `BlackOps3.exe` and Steam's build number with it. Add a build there when Steam
  updates the game and upstream supports it.
- Signatures live in `compat/signatures.json`. Make one with
  `tools\dev\re\sigmaker.py` from a dump (`tools\dev\re\dump-module.ps1`, then
  `dump-xrefs.ps1 -Fix`), and check the registry with
  `xerces_cli sig check --dump C:\Users\Ginld\XercesDev\dumps` from the repo
  root. Dumps come from a running client, so its hooks are in them; sigmaker
  wildcards them.
- Start the game for tests with `tools\dev\session\start-game.ps1` (after
  asking): development builds need `XERCES_DATA_PATH` pointing at `data\`
  because no updater installs it, and flags must come before `+set` commands or
  they become part of the dvar's value. The other scripts there read the
  console, run console commands, post keys to the game window only, capture it
  and quit it (`docs/TESTING.md`). `m1-check.ps1` runs the M1 in-game checks
  through `Xerces.exe`; it starts the game only with `-Live`.
- Process tweaks (priority, timer resolution) and early console prints crashed
  the game during startup in earlier tests; apply them only after the main menu.
- On this PC, files that processes started from the Claude app write under
  `%LOCALAPPDATA%` land in a private copy other programs cannot see. Point
  Claude-driven Xerces runs at `XERCES_HOME` (for example
  `C:\Users\Ginld\XercesHome`); the launcher passes it on to the game.

## Working conventions

- Commit small, on a branch, with tests passing. Push only when the user asks.
- Formatting: clang-format (C/C++), stylua (Lua), prettier (JSON, Markdown,
  YAML) as upstream's CI does.
- Structured JSON logs and the `xerces_cli` command interface are the automation
  surface (DESIGN.md 15).
