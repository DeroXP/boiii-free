# Xerces

Xerces is a rename and redesign of the Ezz BOIII client (fork of momo5502's boiii, GPL-3.0) into a
multi-mod launcher and loader for Call of Duty: Black Ops III. Self-hosted and offline play only.

- Design, milestones and decisions: `docs/DESIGN.md` (read section 0 first).
- Research notes with file:line citations: `docs/research/`.
- Fenced code you must not touch, and upstream features Xerces must not build on: `docs/FENCED.md`.

## Current state (2026-11-04)

- Design phase for the client. The upstream base is not chosen yet (DESIGN.md D1; recommended:
  `upstream/beta` plus the two `upstream/mod-stack` commits).
- Branches: `main` is the user's fork (upstream 2026-04-09 plus a 17-commit multi-mod proof of concept);
  `xerces-design` is upstream `main` 5aa7fac plus the design docs and `backend/`. Remotes: `origin` =
  DeroXP/boiii-free, `upstream` = Ezz-lol/boiii-free (branches `main`, `beta`, `mod-stack`).
- A tag named `main` exists, so always write `refs/heads/main` for the fork branch.
- `backend/` is backend slice M5a (FastAPI + Postgres), which the user deploys on Railway. The game client
  does not talk to it yet.

## Build (client)

Requirements: Visual Studio 2022 or its Build Tools with the C++ workload **and** the "C++ Clang tools
for Windows" plus "MSBuild support for LLVM (clang-cl) toolset" components. Upstream `main` builds only
with clang-cl; MSVC `v143` rejects its flags. Upstream `beta` moved to premake 5.0.0 + ninja + LLVM, so
these steps change once the base is chosen.

```
git submodule update --init --recursive
premake5 vs2022
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" build\boiii.sln -m -p:Configuration=Release -p:Platform=x64
```

Do not use `build.bat` in automation (it ends with `pause`). Output: `build\bin\x64\Release\`.

## Backend

```
cd backend
<python> -m venv <a folder outside the repo>
<venv>\Scripts\python -m pip install -r requirements.txt -r requirements-dev.txt
<venv>\Scripts\python -m pytest -q
```

Deployment steps are in `backend/README.md`. Keep one replica (presence and rate limits are in memory).
Railway settings live in its dashboard: Railway's `railway.json` is deprecated, new services cannot use
it, and existing ones stop reading it on 2026-12-01. Behind Railway's edge the client address is the first
`X-Forwarded-For` entry (`XERCES_TRUST_PROXY=1`). Railway's docs do not say this, so re-check it with the
README's forged-header test after deploys. Tests run on SQLite. For PostgreSQL there is no install on this
PC; earlier runs used the `pgserver` wheel in a throwaway venv.

## Hard rules

- Never modify fenced code (`docs/FENCED.md`): Steam ownership and licence handling, auth tickets,
  anti-tamper (Arxan), TAC workarounds, fastfile authentication. If a feature seems to need it, ask.
- No features that touch official matchmaking or Demonware. Demonware emulation is frozen.
- Workshop content only through the user's Steam client. Never call or extend upstream's anonymous
  downloaders (SteamCMD, Steam CM/CDN, FastDL v1), never add a new download path for Workshop content, and
  never transfer it between players.
- Xerces code never hardcodes game offsets. Use the signature registry (pattern scans resolved at
  `post_unpack`). Never write code bytes without verifying the whole expected instruction sequence. Never
  scan or patch the second `.text` section.
- No suppressed fatal errors: never return from `Com_Error`. Fix causes.
- Never launch the game, or anything that starts it, without asking the user first; they may be playing.
- **Never launch a build whose updater is not disabled.** Upstream's Release updater replaces any renamed
  exe with upstream `boiii.exe` and deletes unknown files under `%LOCALAPPDATA%\boiii`, and its forced
  first-run update ignores `-noupdate`.
- Memory dumps of the game go to `C:\Users\Ginld\XercesDev\dumps` (outside OneDrive and AppData), never into
  the repo. Xerces adds no game binaries or Activision data to the repo, fixtures or CI; inherited upstream
  files follow DESIGN.md D17.
- Xerces docs and UI describe Xerces as a client for people who own BO3 on Steam; never advertise play
  without ownership.
- Keep upstream credits and the GPL-3.0 licence. Name namespaces, libraries, DLLs, executables other than
  `Xerces.exe` and script builtins `xerces_*`, never a bare `xerces` (avoids clashing with the Apache Xerces
  XML library).

## Facts that save time

- The installed `BlackOps3.exe` is the September 2026 Steam build: PE checksum `0x6531394`, build id
  24784313. Its code is encrypted on disk, so static analysis needs a live-process memory dump.
- Upstream symbols are `{new client, legacy client, server}` triples picked by PE checksum, and the client
  refuses unknown checksums. A Steam update stops the whole client until upstream ships new tables.
- MinHook allows one detour per address and upstream's `detour::create` throws on a second; shared targets
  go through the Xerces hook bus (DESIGN.md 3.4).
- `upstream/mod-stack` implements additive multi-mod loading (`docs/mod-stack.md` on that branch). The
  proof of concept's "queue-add" function is most likely a script-string routine. Do not call it.
- Process tweaks (priority, timer resolution) and early console prints crashed the game during startup in
  earlier tests; apply them only after the main menu.
- On this PC, files that processes started from the Claude app write under `%LOCALAPPDATA%` land in a
  private copy other programs cannot see. Point Claude-driven Xerces runs at `XERCES_HOME` (for example
  `C:\Users\Ginld\XercesHome`) once that override exists.

## Working conventions

- Commit small, on a branch, with tests passing. Push only when the user asks.
- Formatting: clang-format (C/C++), stylua (Lua), prettier (JSON, Markdown, YAML) as upstream's CI does.
- Structured JSON logs and the `xerces_cli` command interface are the automation surface (DESIGN.md 15).
