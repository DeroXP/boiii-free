# Testing Xerces

What runs by itself, and the checklist for what only the real game on your PC
can show (docs/DESIGN.md 15.3 and 15.4). The checklist grows every milestone.

## Automated

| What                                                                                                                                                             | Command                                                        | Needs                         |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- | ----------------------------- |
| Core unit tests: manifests, SemVer, the planner and its determinism, merges, conflict rules, hook chains, packs and share codes, logs, the loose both-apply pair | `tools\build.ps1 -Tests`                                       | LLVM 23, Ninja, premake 5.0.0 |
| CLI smoke: plan, conflicts and pack round trip on the fixtures                                                                                                   | `tools\build.ps1 -Tests`                                       | the same                      |
| Backend: auth, rate limits, privacy, migrations                                                                                                                  | `cd backend` then `pytest -q`                                  | Python 3.12+                  |
| Formatting: prettier, clang-format, stylua                                                                                                                       | `yarn run check`, `clang-format --dry-run`, `stylua --check .` | Node, LLVM, StyLua            |

The fixtures in `tests/fixtures/` are written for Xerces and contain no game
data.

## Manual checklist

Tick each item with the date and build. Never run a build whose updater is not
disabled (commit 62e28935 disables it).

### Before the first game session (M0)

- [ ] The Visual Studio Build Tools have the "C++ ATL for latest v143 build
      tools (x86 & x64)" component. Without it the client does not compile:
      every client file includes `atlbase.h` through `std_include.hpp`.
- [ ] `tools\build.ps1 -Client` builds `build\bin\x64\Release\boiii.exe` with no
      errors. This is the first compile of the mod-stack merge (commit 6bc02cb4)
      and of the updater change.
- [ ] The built exe does not contact upstream's update server on start (watch
      the console; no "update" lines) and leaves `%LOCALAPPDATA%\boiii` alone.

### Supervised game session (D3, with you at the PC)

Each item records what happened in the re-verification ledger (DESIGN.md 2.3).

- [ ] Vanilla launch reaches the main menu.
- [ ] `+set fs_game <mod>` loads a single Workshop mod.
- [ ] Stacking: Solo Easter Eggs with DS4C and a third mod through
      `+set fs_extramods`; the zombies menu shows all three, and each mod's
      feature works in a match.
- [ ] Joining a server whose mods you lack names the missing mods instead of
      offering a download (the Xerces change in `check_valid_mod_stack`).
- [ ] Two fastfiles that share a name (for example two mods with `core_mod.ff`):
      note what PMem does.
- [ ] `en_`-prefixed zones of a stacked mod load.
- [ ] The queue-add falsification test (DESIGN.md 2.3, row 7).
- [ ] Lua DLL load paths of the stacked mods.
- [ ] Uncapped performance baseline (`com_maxfps 0`, vsync off, PresentMon).
- [ ] A memory dump to `C:\Users\Ginld\XercesDev\dumps` for the first signatures
      of mod-stack's targets.

### Loader and manifest (M1)

- [ ] A plan with two Workshop mods loads through the CLI (`xerces_cli load`).
- [ ] A forced crash leads to safe mode on the next start.
- [ ] Load, unload and reload work from the CLI in the front end.

### Conflict system (M2)

- [ ] `xerces_cli test run both-apply` with the loose pair: the merged table
      shows both rows and both edits, and `xerces_test_hooks` reads
      `fixture.alpha;fixture.beta;probe;`.
- [ ] The same with the fastfile pair built with the BO3 Mod Tools.
- [ ] The pre-launch conflict report matches what happens in the game for two
      legacy mods that replace the same script.
