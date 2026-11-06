# Testing Xerces

What runs by itself, and the checklist for what only the real game on your PC
can show (docs/DESIGN.md 15.3 and 15.4). The checklist grows every milestone.

## Automated

| What                                                                                                                                                             | Command                                                        | Needs                         |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- | ----------------------------- |
| Core unit tests: manifests, SemVer, the planner and its determinism, merges, conflict rules, hook chains, packs and share codes, logs, the loose both-apply pair | `tools\build.ps1 -Tests`                                       | LLVM 23, Ninja, premake 5.0.0 |
| CLI smoke: plan, conflicts and pack round trip on the fixtures                                                                                                   | `tools\build.ps1 -Tests`                                       | the same                      |
| CLI smoke on a home: mods enable, disable, order and info, `plan apply --dry-launch`, `packs apply`, `logs tail`, `sig check` on a fake game folder              | `tools\build.ps1 -Tests` (`tools\ci\cli_smoke.ps1`)            | the same                      |
| Benchmark: frame statistics, Welch, TOST, the bootstrap, the probe/PresentMon/console/log readers; `bench` on small logs and CSVs                                | `tools\build.ps1 -Tests`                                       | the same                      |
| Signature registry against a stored dump (`compat/signatures.json`)                                                                                              | `xerces_cli sig check --dump C:\Users\Ginld\XercesDev\dumps`   | a dump on this PC             |
| Pre-flight check of the installed game against `compat/builds.json`                                                                                              | `xerces_cli game check`                                        | the game on this PC           |
| Hashing micro-benchmark: disk read speed apart from hashing speed, the portable code against CNG, files hashed 1 to 6 at once (reads only)                       | `xerces_tests --bench-hash <file>...`                          | large files (Workshop)        |
| Launcher start times: 20 dry starts and one through the boot screen, summarized from the launcher log                                                            | `tools\dev\bench\launcher-bench.ps1`                           | a seeded home                 |
| Backend: auth, rate limits, privacy, migrations                                                                                                                  | `cd backend` then `pytest -q`                                  | Python 3.12+                  |
| Formatting: prettier, clang-format, stylua                                                                                                                       | `yarn run check`, `clang-format --dry-run`, `stylua --check .` | Node, LLVM, StyLua            |

The fixtures in `tests/fixtures/` are written for Xerces and contain no game
data.

## Manual checklist

Tick each item with the date and build. Never run a build whose updater is not
disabled (commit 62e28935 disables it).

### How a session runs

The scripts in `tools\dev\session\` act on the running `boiii.exe` only:

- `start-game.ps1 -GamePath <Black Ops III folder> [-Mod <id>] [-ExtraMods <id>,<id>] [-Safe]`
  starts this repository's build from the game folder, with `XERCES_DATA_PATH`
  pointing at `data\`. It puts flags before the `+set` commands, because the
  engine joins everything after `+set <dvar>` up to the next `+` into the value.
- `console-text.ps1` reads the console window; `console-command.ps1 <command>`
  runs one console command and prints what it logged.
- `send-key.ps1 <key>` posts one key to the game window. System dialogs never
  receive it.
- `capture.ps1 -Path <png>` saves a picture of the game window even when
  something covers it; add `-Screen` while a video plays, which the window
  capture shows as black.
- `stop-game.ps1` quits through the console, the way closing it does.

`tools\dev\re\dump-module.ps1` saves the game's in-memory image to
`C:\Users\Ginld\XercesDev\dumps` (never into the repository, OneDrive or
AppData), and `dump-xrefs.ps1` lists call sites in a dump and writes a copy that
`llvm-objdump` disassembles by game address.

A new `boiii_players` profile asks three first-run questions (a title screen,
graphic content, brightness); Enter takes the defaults.

### Before the first game session (M0)

- [x] (2026-11-04) The Visual Studio Build Tools have the "C++ ATL for latest
      v143 build tools (x86 & x64)" component. Without it the client does not
      compile: every client file includes `atlbase.h` through `std_include.hpp`.
- [x] (2026-11-04, 2761e2d8) `tools\build.ps1 -Client` builds
      `build\bin\x64\Release\boiii.exe` with no errors. This is the first
      compile of the mod-stack merge (commit 6bc02cb4) and of the updater
      change.
- [x] (2026-11-04, 815545d3) The built exe does not contact upstream's update
      server on start: no update lines in the console logs of the three D3 runs.
- [ ] What the build writes under `%LOCALAPPDATA%\boiii` moves to `<home>` in M3
      (DESIGN.md 3.5). Seen on 2026-11-04: the folder did not exist before the
      session. The first start created upstream's identity key pair in `user\`
      (written by the fenced `auth.cpp`), `user\game_path.txt`, an empty
      `minidumps\`, and `cache\mod_previews\` with 17 images. Eight of those
      come from the items' own `previewimage` files; nine were fetched through
      `api.steampowered.com` because those items ship no preview. Nothing was
      deleted. The build also writes `%TEMP%\boiii-tlsdll.dll`.

### Supervised game session (D3)

Run on 2026-11-04 by Claude on your PC while you were away, with your go-ahead.
Build 815545d3 (the screen shows `v3.0.0-499-gdbd2148d`: the same code just
before it was committed), Steam build 24784313 of `BlackOps3.exe` (checksum
`0x6531394`; it reports CL 13892626, compiled 15 December 2025), windowed
(`-safe`), offline only. Logs and pictures are in
`C:\Users\Ginld\XercesDev\launch\`. The ledger (DESIGN.md 2.3) has the results
too.

- [x] Vanilla launch reaches the main menu (title screen after about 50 s).
- [x] `+set fs_game 1944930083` loads one Workshop mod (Solo Easter Eggs) from
      `steamapps\workshop\content\311210\1944930083`: `ModLoad done` and its
      `core_mod` zone. A flag after `+set` becomes part of the value: with a
      trailing `-safe`, `fs_game` was `1944930083 -safe`, nothing loaded, and
      the log still said `ModLoad done`.
- [ ] Stacking: Solo Easter Eggs as `fs_game`, with DS4C Patched (3171632315)
      and Classic Viewmodel FOV (3656779213) through `+set fs_extramods`. Done:
  - All three load: `core_mod`, `stack_3171632315_core_mod`,
    `stack_3656779213_core_mod`, and `stack_<id>_zm_mod` for the two mods that
    have one.
  - The main menu and the Zombies lobby show DS4C's art, so a stacked mod's
    assets replace stock ones.
  - An offline solo match on Shadows of Evil loaded and ran, and `disconnect`
    returned to the lobby.
  - `loadmod 3171632315` replaced the main mod while the stack was loaded.

  Open: the loaded-mod label ("Ã¢â‚¬Â¦ and 2 more") was not checked, and each
  mod's feature in play needs a person playing.

- [ ] Joining a server whose mods you lack names the missing mods instead of
      offering a download (the Xerces change in `check_valid_mod_stack`). Not
      run: it needs a server.
- [x] Two fastfiles that share a name: all three mods ship `core_mod.ff`. With
      mod-stack's aliases PMem raised no error in three cases: the load, a front
      end Ã¢â€ â€™ match Ã¢â€ â€™ front end round trip, and a main-mod switch
      that unloaded every stacked zone in reverse order.
- [x] `en_`-prefixed zones of a stacked mod load (`en_stack_<id>_core_mod`,
      `en_stack_<id>_zm_mod`).
- [x] The queue-add falsification test (DESIGN.md 2.3, row 6), on this session's
      dump. The candidate (new `0x1412D8830`) is `SL_GetStringOfSize`:
      `SL_GetString` (`0x1412D7B40`) measures the string and tail-jumps to it,
      and `SL_GetLowercaseString` (`0x1412D7A30`) calls it with a lower-cased
      copy. It has 16 call sites (14 calls and 2 tail jumps, 4 of them at
      `0x1422xxxxx`), the PoC's count. The "runner" `0x1412D7F30` is called with
      `ecx=2` by `0x1412C7F40`, the routine that clears `animtree_loading`, and
      by `0x1412C8000`, and with `ecx=1` by `0x1412C8040`. That is the engine's
      string-table shutdown for one user. Neither is an autoexec registrar.
- [x] Lua DLL load paths of the stacked mods: only DS4C ships a DLL
      (`T7Overcharged.dll`, in the item's root folder). It was never loaded into
      the process: not in the front end, not in the match, and not after DS4C
      became the main mod.
- [ ] Uncapped performance baseline (`com_maxfps 0`, vsync off, PresentMon). Not
      run yet. PresentMon 2.4.1 is installed (Performance, below); capturing
      needs an elevated terminal and your go-ahead for the game session.
- [x] A memory dump:
      `C:\Users\Ginld\XercesDev\dumps\BlackOps3-20261104-223001.bin` (the 471 MB
      in-memory image of `BlackOps3.exe`), with a `.json` sidecar and a `.pe`
      copy for `llvm-objdump`. The first signatures for mod-stack's targets were
      made from it: `compat/signatures.json` has nine entries, and
      `xerces_cli sig check --dump C:\Users\Ginld\XercesDev\dumps` resolves all
      nine at upstream's addresses.

Also seen:

- With any mod loaded, the log has 123 lines of
  `Playlist error: line N: 'rule' found outside of any gametype or playlist block`.
  There are none without a mod. This is not yet compared with upstream's build.
- In the match, one script tried to set a cheat-protected dvar
  (`cg_thirdPersonAngle`). The game refused it and nothing else happened.
- Windows asked whether "BOIII" (publisher Ezz) may use public and private
  networks. The prompt was left for you to answer; offline play does not need
  it.
- The client created `boiii_players` in the game folder. The official `players`
  folder was not touched.

### Launcher spike (D4, the M3 gate)

Run on 2026-11-05 by Claude on your PC while you were away; results and the open
decision are in DESIGN.md 7.1.

Third-party sources live outside the repository until the gate decides, in
`C:\Users\Ginld\XercesDev\third_party` (downloaded to `..\downloads`):

| File                      | Source                                     | Bytes     | SHA-256                                                            |
| ------------------------- | ------------------------------------------ | --------- | ------------------------------------------------------------------ |
| `RmlUi-6.3.zip`           | github.com/mikke89/RmlUi, tag 6.3 (MIT)    | 4,513,038 | `bfb340660e4bce38d4211cffc836cfbe1be676aa36ef9cc48e7df3855ee74fe3` |
| `freetype-2.14.3.tar.xz`  | download.savannah.gnu.org (FTL or GPLv2)   | 2,670,220 | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` |
| `InterTight-wght.ttf`     | github.com/google/fonts, `ofl/intertight`  | 581,588   | `b81b73dcb64df3c230cabade7df6c5773bf863233f24c9ee51087519f1f88b6f` |
| `IBMPlexMono-Regular.ttf` | github.com/google/fonts, `ofl/ibmplexmono` | 135,580   | `6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46` |
| `IBMPlexMono-Medium.ttf`  | github.com/google/fonts, `ofl/ibmplexmono` | 136,704   | `a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755` |

The fonts and their OFL texts are in `data/xerces/fonts/` (unmodified; the
variable font's file name drops the `[wght]` suffix).

To repeat the measurements (the programs are not the game; they only draw a
window):

```
tools\build.ps1 -Spike -ToolsDir C:\Users\Ginld\XercesDev\tools
build\bin\x64\Release\xerces_spike_dx11.exe --scale 1.5 --size 1200x680 --seconds 4 --log dx11.log
build\bin\x64\Release\xerces_spike_gl3.exe --async --seconds 2 --log gl3.log
build\bin\x64\Release\xerces_spike_dx11.exe --animate --seconds 8 --log animate.log
build\bin\x64\Release\xerces_spike_dx11.exe --script tools\dev\spike\pad-script.txt --seconds 7 --log pad.log
```

The log has a `startup:` line per step, the first-frame time, and frame-time
statistics. `tools/dev/spike/home-reference.html` is the mockup with the same
fonts, for comparison in a browser (Edge headless:
`msedge --headless=new --force-device-scale-factor=2 --window-size=1200,680 --screenshot=ref.png <file URL>`).

- [x] Typography at 100, 150 and 200 percent against the mockup. One gap: no
      OpenType (GPOS) kerning with RmlUi's default font engine.
- [x] Transitions: worst frame 5.0 ms (DX11) and 5.3 ms (GL3) while switching
      modes, inside the 144 Hz budget.
- [x] First frame, as restated on 2026-11-05 (DESIGN.md 7.1): native paint under
      100 ms (21 ms) and first RmlUi frame under 250 ms from a warm start (190
      to 216 ms). The original 150 ms is not reachable: GPU driver start-up
      takes 115 to 145 ms. Cold first launches measured 262 to 415 ms.
- [x] Controller navigation through the scripted pad on both renderers.
- [ ] A physical controller press (the Tarantula) and vsync pacing on a live
      display: need you at the PC.
- [x] GL3 and DX11 both run.
- [ ] Proton: not run.

### Build switches D14 and D15 (M0)

- [x] (2026-11-04) The default build (both switches off) reaches the main menu.
      `workshop_download` is an unknown command, and the exe no longer contains
      SteamCMD's download command, the Steam CM host, the FastDL code or Ezz's
      Discord application id.
- [ ] Joining a server whose mod or map you lack names it and offers to open its
      Workshop page in Steam, which then downloads it.
- [ ] `friends_open` and inviting a friend say the feature is off in this build.
      Seen: `friends_open` printed nothing to the console; the message is a
      toast, not checked on screen.
- [ ] With `premake5 ninja --with-ezz-social --with-upstream-downloaders` the
      client builds and behaves as upstream (only when a comparison is needed).
- [ ] Plugins (DESIGN.md 5.9, built 2026-11-05): with a DLL in
      `<game>\boiii\plugins`, the default build does not load it and
      `boiii_players\plugins.log` says how many DLLs it left alone;
      `--with-upstream-plugins` builds upstream's loader back in.

### Upstream downloads removed (D9, M0)

Removed on 2026-11-05: the archive.org `BlackOps3.exe` swap, the old launcher's
version picker and Patches tab, the dedicated server's `-beta` self-download and
the `beta_data` folder that went with them. `-beta` is still accepted, so
existing server command lines work, and does nothing. Official DLC maps get a
notice instead of upstream's download page.

- [x] (2026-11-05, branch `launcher/d9-remove`) The default build's `boiii.exe`
      no longer contains the archive.org URLs, upstream's beta server path,
      Ezz's DLC forum page or the version picker's callbacks.
- [ ] Joining a server on one of the game's own maps that your game lacks (a DLC
      Zombies map you have not installed, for example) shows "DLC needed": the
      server uses DLC you don't own or haven't installed, with the map's name.
      Yes opens Black Ops III's store page in the Steam client; No closes the
      notice. The join stops either way and nothing is downloaded. The notice
      also says to verify the game files in Steam when the map comes with the
      base game.
- [ ] A `BlackOps3.exe` the client cannot load (after a Steam update) shows
      "Xerces does not support this Black Ops III build" with no download offer
      and leaves the exe as it is (only when Steam updates the game).
- [ ] Upstream's old launcher (`boiii.exe` without `-launch`, with
      `XERCES_DATA_PATH` set to the repository's `data\`) has no version picker
      and no Patches tab, and PLAY still starts the game. The launcher's HTML
      and JS come from the data folder, not from `boiii.exe`: a data folder that
      upstream's updater installed (`%LOCALAPPDATA%\boiii\data`) still shows the
      picker and the Patches tab until Xerces ships its own data (D16). There,
      PLAY with a picked version still starts this build and downloads nothing.

### Loader and manifest (M1)

What the game role does since 389dff95 (`src/client/xerces`): it resolves
`compat/signatures.json` against the running game before mod_stack hooks
anything (multi-mod stays off on a miss), reports it in
`<home>\cache\signatures-report.json`, checks `fs_game` and `fs_extramods`
against the plan file named by `-xerces-plan`, and moves the journal
(`<home>\logs\journal.json`) through `symbols`, `frontend`, `loading_map`,
`in_game` and `clean_exit` (DESIGN.md 5.6 has when each is written). Its log is
`<home>\logs\<date>-<time>-game-<pid>.jsonl`.

`tools\dev\session\m1-check.ps1 -Case <case> -Live` runs these items one case
per run through `Xerces.exe` (or `xerces_cli`) and prints pass or fail per check
(journal phases, game-log events, `fs_game` and `fs_extramods` from the console,
the launcher's view of the exit, captures of Home). Cases: `vanilla`, `modded`,
`crash`, `restart` (after `crash`), `safeoff` (after `crash`), `reopen`,
`dvars`, `cli [-CliReload]` and `cliapply`. It exits 1 when a check fails.
Without `-Live` only `vanilla`, `modded` and `crash` run, as dry launches. Its
home is `C:\Users\Ginld\XercesHome-m1`.

The first live run was on 2026-11-05 (the user's go-ahead), Steam build
24784313, with the D3 stack as the Zombies profile. `vanilla` and `modded` ran
on 4bf21880, the rest on 3a7266c6 (which adds `loader.journal.none` and changes
only the path of a game started without a launch id). Every case passed; what
each one does not cover is said under its item.

- [x] (2026-11-05, 4bf21880) Launch vanilla from `Xerces.exe` reaches the menu.
      The game log shows `xerces.signatures.resolved` (9 of 9) and the journal
      reads `frontend`; quitting from the menu leaves `clean_exit`, and the
      launcher sees exit code 0.
- [x] (2026-11-05, 4bf21880) Launch modded with the D3 stack (1944930083, then
      3171632315 and 3656779213): `xerces.feature.on` and `loader.plan.applied`
      in the game log, the console's `fs_game` and `fs_extramods` match the
      plan, and quitting leaves `clean_exit`. Not looked at: the mods' content
      in a match (D3 did that with the same stack).
- [x] (2026-11-05, 3a7266c6) A forced crash leads to safe mode on the next
      launcher start, and "Launch with mods off" reaches the menu without mods:
      `boiii.exe` was ended from outside at the menu (not during a match); the
      launcher logged the exit (code 4294967295), the journal stayed at
      `frontend`, Home offered safe mode naming the 3 mods. "Launch with mods
      off" started `-launch -nointro` with no `fs_game`, the journal recorded
      `safe_mode: true`, the game logged `loader.start` with no plan, and after
      quitting Home offered safe mode no more. Not run: a crash during a match.
- [x] (2026-11-05, 3a7266c6) After that crash, a game started the way the crash
      dialog's "Restart game" starts it (`XERCES_HOME`, no launch id) logs
      `loader.journal.none`, writes no phases, and leaves `journal.json`
      byte-for-byte unchanged, also after it quits. The first run (on 4bf21880)
      left the journal alone too, but logged nothing about it, so 3a7266c6 adds
      `loader.journal.none`. Not run: the real crash dialog's button.
- [x] (2026-11-05, 3a7266c6) Close `Xerces.exe` while the game runs and open it
      again: the launcher closed normally and the game kept running (its entry
      `game-<pid>.json` in `<home>\run`); the reopened launcher offered no safe
      mode, refused a second Launch ("The game is already running."), left the
      journal alone, and logged `launcher.game.gone` when the game quit. Home
      does not show that the game is running until Launch is pressed.
- [x] (2026-11-05, 3a7266c6) Settings changed in the launcher win over the
      game's saved config: with `"dvars": { "cg_fov_default": "90" }` and
      config.cfg at 80, the command line had `+set cg_fov_default 90`, the game
      log `xerces.dvars.session` and `xerces.dvars.reapplied`, and the console
      `cg_fov_default` 90. After quitting, config.cfg was identical to its
      backup (`cg_fov_default "80"` kept). Not run: changing the setting in the
      game itself (`xerces.dvars.changed_in_game`).
- [ ] Find the culprit: one test launch per step, each result recorded when the
      game exits (checked with written journals only, see Launcher below).
- [x] (2026-11-05, 3a7266c6) With the game in the menus: `xerces_cli status`
      shows the build (0x6531394), the plan, `fs_game`, `fs_extramods`,
      `multi_mod: true` and the phase (it finds the game's pipe in
      `<home>\run`); `xerces_cli load --extras 3171632315,3656779213` and
      `xerces_cli unload` answered and the game logged `ipc.reload` for each,
      then quit with `clean_exit`. The pipe itself is tested in `test_ipc.cpp`
      (a served pipe, a forged run file refused). Not run: the same in a match,
      where they should answer that the stack only changes in the menus; and
      nobody looked at the mods menu after the reload.
- [ ] Asset pools (DESIGN.md 5.4 step 4; the report is built, the in-game sizing
      is not): launch a stack with a mod whose `assetlimits.txt` or
      `assetpools.txt` raises pools. After the mod loads, the client's console
      lines `Reallocating asset pool type <n>: <old> -> <new> entries` should
      name the pools and sizes the plan file's `asset_pools.stack` (or
      `xerces_cli plan resolve ... --game-dir <game>`) says the stack raises,
      and a pool on the file's first line should be missing from both
      (upstream's parser never counts that line). Then load a custom map that
      ships a pool file, with the map in the plan: its pools should print the
      same lines when the map loads, as `asset_pools.stack` says.
- [x] (2026-11-05, offline, dry launches only) Zone memory (DESIGN.md 5.3
      step 5) on the real Workshop folder, read-only, with a copy home
      (`C:\Users\Ginld\XercesHome-zonemem`, the warm hash cache of
      `XercesHome-maps`; the cache was byte-identical afterwards, so nothing was
      hashed) and the Zombies profile, through
      `xerces_cli plan apply --dry-launch`: DS4C alone takes 451.3 MiB (1300.3
      of 2048 MiB before a map) and the stack 1944930083 + 3171632315,3656779213
      451.4 MiB (1300.4); both name exactly BACKROOMZ (2461.4 MiB, 413.4 over)
      and Super Mario 64 (2112.8 MiB, 64.8 over). 1944930083 + 3656779213 takes
      0.1 MiB (849.1) and every map fits. DS4C with Super Mario 64 as the
      profile's map is refused (`plan.memory.over`, exit 2).
      `xerces_cli plan resolve --mods-dir <Workshop> --game-build 0x6531394`
      prints the same numbers; in Multiplayer it says the check is skipped. A
      scripted capture of Mods (`check-screens.ps1 -Views mods`) shows the
      warning as the plan note.
- [x] (2026-11-05, offline) Zone memory after review, with `xerces_cli` on a
      header-only copy of the Workshop folder (each item's `workshop.json` and
      the first 0x100 bytes of each `.ff`, outside the Steam folders): with no
      mods, a 1300 MiB test map gets `plan.memory.maps_too_big` ("Play another
      map", no "Remove a mod"); with DS4C it is listed apart from Super Mario
      64, which keeps `plan.memory.maps_over`. With BACKROOMZ's main zone cut
      short in the copy, it is named as not checked (`maps_unread`,
      `plan.memory.maps_unread`) instead of fitting. `packs import` and
      `conflicts check --pack` with `--game-build 0x6531394` refuse a DS4C +
      Super Mario 64 pack (`plan.memory.over`, exit 2).
- [ ] Zone memory in the game: with DS4C, Xerces warns about Super Mario 64 and
      BACKROOMZ; load one of them anyway (`tools\dev\session\map-check.ps1`,
      after asking) and confirm the game still exits while the map loads (done
      2026-11-05: BACKROOMZ ran out of memory at the zone allocator's check, as
      predicted; the other maps loaded with DS4C earlier the same day), then
      load the largest map it calls fine, SANCTUM OBITUS (`zm_log_overunder`:
      Xerces prints 1994.2 MiB with DS4C, game 849 + mods 451.3 + map 693.9,
      about 54 MiB to spare; it loaded with DS4C alone before this check
      existed, DESIGN.md 6.10), and Super Mario 64 without DS4C (1661.5 MiB),
      and confirm both load. RETURN TO HOGWARTZ (`zm_wizardcastle`, 1911.6 MiB
      with DS4C) is an extra case with more room. Re-measure the Zombies
      baseline in another game language and on another stock map's lobby, and
      measure Multiplayer and Campaign (a dump before a custom map's main zone
      loads), then fill `compat/zone_memory.json`.
- [x] (2026-11-05, 24bd2f9b) Out of memory in the game, as described below:
      BACKROOMZ with DS4C alone showed "Black Ops III ran out of memory" naming
      `zm_backrooms` and DS4C Patched v0.85, the remove-a-mod advice and the
      Mods screen pointer; after a click on OK (BM_CLICK on the box's button)
      the game ended by itself with 0xFFFFFFFF, no crash dump, no
      `loader.exit.unexpected`; `loader.out_of_memory` had zone and map
      `zm_backrooms`, phase `loading_map`, the mod, caller 0x20C5A3D (the zone
      allocator's bounds check), and the journal stayed at `loading_map`. (A
      first run posted WM_COMMAND to the box, which did not close it; map-check
      now clicks the button.) Not run yet: Super Mario 64 and a fitting map on
      this build.
- [ ] Out of memory (DESIGN.md 5.6, feature `oom_report`; built 2026-11-05):
      DS4C alone, then `map zm_backrooms`, through
      `tools\dev\session\map-check.ps1 -Map zm_backrooms -Stack 3171632315 -Live`.
      The run should end `crash_load` with the dialog "Black Ops III ran out of
      memory" captured (`dialog-load.png`), naming `zm_backrooms` and DS4C
      Patched v0.85 and saying to remove a mod or play another map, and that the
      Mods screen lists the custom maps that do not fit. map-check then presses
      OK, and the game should end by itself within a second with exit code
      0xFFFFFFFF (`exit_code` in `results.jsonl`; the launcher logs
      `launcher.game.exited` with 4294967295). The game log should have
      `xerces.signatures.resolved` (10 of 10), `xerces.feature.on` for
      `oom_report`, and `loader.out_of_memory` with `zone` `zm_backrooms`, `map`
      `zm_backrooms` (not `core_frontend`, which `mapname` still says then), the
      mods, `phase` `loading_map`, a `caller` and the console lines; no
      `loader.exit.unexpected` and no new `crash-*.dmp` (the run's `dumps` stays
      empty); the journal stays at `loading_map`, and the next launcher start
      offers safe mode. `zm_supermario64` should do the same. Then a map that
      fits with DS4C (`zm_log_overunder`) to see that nothing else changed. The
      no-mods text (`fs_game` `usermaps`) and the text with no zone loading are
      covered by the unit tests only: no installed map overflows the pool alone.
- [x] (2026-11-05, 3a7266c6) `xerces_cli plan apply` (no `--dry-launch`), from
      the build with `boiii.exe` next to `xerces_cli.exe`, with the D3 stack as
      the Zombies profile and `XERCES_HOME` outside AppData: the game started
      with the same command line as Launch modded, took the journal over, logged
      `loader.plan.applied`, and the console's `fs_game` and `fs_extramods`
      matched the plan. This is M1's "a plan with two Workshop mods loads
      through the CLI". Afterwards `xerces_cli sig check` read the report the
      game wrote (9 of 9, exit 0) and `xerces_cli logs tail --role game` showed
      its log.
- [x] (2026-11-05, dry launches only) `xerces_cli plan apply --dry-launch` on a
      copy of the D3 profile (`C:\Users\Ginld\XercesHome-cli`) prints
      `-launch -nointro -xerces-plan <id> +set fs_game 1944930083 +set fs_extramods 3171632315,3656779213`
      with the plan id `Xerces.exe`'s dry launch logged on the same home, writes
      the plan file and no journal.

### Mods on custom maps (2026-11-05)

`tools\dev\session\map-check.ps1 -Map <map> [-Stack <ids>] -Live` starts the
game through `Xerces.exe` (Launch modded or vanilla), runs `map <map>` from the
console, waits for the journal's `in_game`, plays, disconnects and quits; it
records crash dialogs, freezes, error drops and timeouts, keeps the console and
the game log, and copies crash dumps to
`C:\Users\Ginld\XercesDev\dumps\map-checks`. Results are in
`C:\Users\Ginld\XercesDev\map-checks\results.jsonl`. `-SkipCinematic` skips a
level's intro as a player would; `-ExtraFlags -log-script-errors` prints script
runtime errors. Home `C:\Users\Ginld\XercesHome-maps`, Steam build 24784313, the
user's eight installed Workshop maps; "the stack" is the D3 stack (Solo Easter
Eggs 1944930083 main, DS4C 3171632315 and Classic Viewmodel FOV 3656779213
extra).

- [x] Before map-first (c57ba507): with the stack, Cheese Cube, RUST V5 and
      PROJECT BLUNDEEZY played; SANCTUM OBITUS dropped with a GSC link error;
      PROJECT: KABOOM and RETURN TO HOGWARTZ froze in level-init scripts; Super
      Mario 64 and BACKROOMZ ended the game while loading. Every failure
      reproduced with DS4C alone and none with Solo Easter Eggs + FOV: DS4C's
      perk scripts replaced the maps' own (SANCTUM, KABOOM), and DS4C's zones
      leave too little zone memory for the two biggest maps (below). With no
      mods every map played.
- [x] With map-first (17a929d2's code, the build before its review fixes and the
      build after): SANCTUM and KABOOM play with DS4C alone (15 and 36
      `loader.script.map_first` decisions, all perk scripts); with the stack,
      Cheese Cube, SANCTUM, KABOOM, RUST, BLUNDEEZY and HOGWARTZ all load, play
      30 s and return to the menu. Shadows of Evil (stock, no map-first
      decision) plays with the stack when its intro is skipped
      (`-SkipCinematic`, 30.9 s to the match).
- [x] Super Mario 64 and BACKROOMZ with DS4C (alone or stacked): the engine runs
      out of its 2 GiB zone-memory pool while the map's main zone loads (dumps
      and the maps' .ff headers: DS4C's zones take about 450 MiB, the maps need
      813 and 1160 MiB on a pool at about 1299 MiB), its out-of-memory handler
      calls `exit(-1)` on the loading thread, and the main thread then crashes
      on objects the exit already destroyed. The journal now records it
      (`loader.exit.unexpected`, 0xFFFFFFFF while `loading_map`) and the crash
      handler leaves a dump. Not fixed: these two maps cannot load with DS4C
      unless the pool grows.
- [ ] Larger zone memory (`-xerces-db-pool`, DESIGN.md 5.4 Zone memory; built
      2026-11-05, never run in the game). One run, with your go-ahead, DS4C
      alone on BACKROOMZ:
      `tools\dev\session\map-check.ps1 -Map zm_backrooms -Stack 3171632315 -ExtraFlags -xerces-db-pool -Live`.
      In the game log (`<home>\logs\<date>-<time>-game-<pid>.jsonl`, copied to
      the run's folder):
  - `xerces.db_pool.installed` with both addresses. Without it,
    `xerces.db_pool.off` says why (a signature or byte check failed) and the run
    proves nothing.
  - `xerces.db_pool` at info, after the first frame: 23 pool calls, the first
    `main` flags 0x1E size 0x80000000 with `size_given` 0x100000000 and
    `reserved` true, the budget 0x140000000 given 0x200000000, and
    `main_size_now` 0x100000000. A warning instead means the hooks were not hit
    or did not apply; its message says which.
  - `xerces.db_pool.usage`: `above_stock` while BACKROOMZ loads, then `in_game`
    at about 2450 MiB (more than the stock 2048).
  - The result is `in_game` with "back to the menu: yes" (leaving frees the
    pages above 2 GiB), and no `loader.exit.unexpected`.
  - If it crashes: before the menu with no `xerces.db_pool` line points at the
    hooks or Arxan; in the dump, a DB thread returning to 0x1420C5A3D means the
    pool is still too small, 0x1420C5AB0 the page budget ran out, and a crash
    after the zone allocation an offset kept in 32 bits. Turn the setting off
    again afterwards either way (map-check writes its own launcher.json, so only
    that home has the flag). The journal then has `"db_pool": true`, and Home's
    "Find the culprit" first runs DS4C alone without the flag (checked in
    `test_m1_runtime.cpp` and `test_launcher_shared.cpp` only).
- [ ] Shadows of Evil with DS4C when its 3.5 minute intro plays to the end:
      DS4C's scripts error on every frame and the local server's reliable
      commands cycle out (`EXE_ERR_RELIABLE_CYCLED_OUT`) before the client is in
      the level. Skipping the intro avoids it. DS4C's own; not checked on plain
      BOIII.
- [ ] A map that ships only one half of a script a mod ships both halves of
      (none among the installed content): the first load logs
      `loader.script.mixed_halves`, the next keeps the mod's pair
      (`loader.script.map_first_skipped`).
- [ ] Joining another host on a custom map: the client follows the host's
      `mapScriptsFirst` (getinfo, lobby content); needs a second machine.
- [x] A map's own HUD over a mod's (`xerces_map_ui_first`): RUST with the stack
      keeps its `t7hud_zm_custom.lua` and `datasources.lua`
      (`loader.ui.map_first`), with no LUI errors (DS4C's copy errored before).
- [ ] A person playing these maps with the stack beyond 30 s.
- [ ] Three runs in a row ended with a fail-fast (0xC0000409, no crash report,
      no exit record) while two full client builds and a 60 GB hash ran in the
      background; the same build passed twice once the machine was idle. Not
      explained; the client now logs an uncaught C++ exception as
      `loader.terminate` before the fail-fast (none was logged since).

### Performance instrumentation (2026-11-05)

What DESIGN.md 12.2 describes was built on `launcher/perf-probe` and covered by
unit tests (rings, CSV, meta, `XERCES_BENCH_AUTO`, the output check); none of it
has run in the game. Each item needs a game start, so each waits for your
go-ahead. Use a home outside AppData (`C:\Users\Ginld\XercesHome-bench`) and an
output folder outside the game and Workshop folders
(`XERCES_BENCH_OUT=C:\Users\Ginld\XercesDev\bench\<session>`). PresentMon 2.4.1
is installed
(`C:\Program Files\Intel\PresentMon\PresentMonConsoleApplication\PresentMon-2.4.1-x64.exe`);
it needs an elevated terminal, which only you can open. Never read the console
during a measurement window.

- [x] (2026-11-05, 5cd56eef, one Cheese Cube run with the D3 stack, which loaded
      in 27.4 s and returned to the menu: the merged build plays)
      `perf.swapchain`: discard (blit) model, 2 buffers, 3840x2160, windowed,
      sync interval 0, `IDXGISwapChain2` available. `perf.startup`: first main
      frame 34.7 s after the process was created, menu ready 41.1 s, the command
      pipe at 17.1 s; the 212 detours were created between 0.08 s and 17.2 s.
      `perf.hooks`: enabling them took 9.2 s in all (about 40 ms each, the
      slowest 77 ms: MinHook's system-wide thread snapshot on a PC with about
      8,100 threads), creating them 2.0 s. About a quarter of the time to the
      first main frame is hook enabling (DESIGN.md 12.4 item 1).
- [x] (2026-11-05, 941b25b, MinHook's thread walk over the game's own threads,
      28d7d494; the same Cheese Cube run with the D3 stack: loaded in 27.2 s,
      played 30 s, back to the menu, journal `clean_exit`, the same 60 script
      errors as before, all from DS4C, no Xerces warnings) `perf.hooks`: 212
      enables took 41.6 ms in all (the slowest 0.44 ms; was 9.2 s), creates 3.7
      ms (was 2.0 s); every detour was created by 7.9 s (was 17.2 s).
      `perf.startup`: first main frame 25.5 s (was 34.7 s), menu ready 31.8 s
      (was 41.1 s); `map-check.ps1` saw the menu 29.9 s after the launch (39.7 s
      the run before).
- [ ] A normal start (no `XERCES_BENCH`) logs `perf.swapchain` once, at the
      overlay's first Present (well before the menu), and `perf.startup` and
      `perf.hooks` once each, after the menu (or after 180 s with
      `menu_timed_out`); `frontend_ms` agrees with the launcher's
      `launch.started` to first main frame (34.0-34.7 s on 2026-11-05) less the
      launcher's own time; `perf.hooks` names how many detours there were and
      how much of the start their enables took. `perf.io` lines appear for
      `journal` and, 10 s after a setting changes, `config.cfg`;
      `perf.load_step` for `hash_names` and the Workshop pools. The start-up
      `journal` lines (`symbols`, then back to `launching`) say
      `"thread":"main"`, and `perf.startup`'s `unpack_thread_id` and
      `main_thread_id` are the same id.
- [ ] The overlay's dialog still works with the idle skip: join a server whose
      map or mod is not installed. The dialog shows, takes the mouse, "Yes"
      opens the item's Workshop page in Steam (now from the main thread), "No"
      closes it, and a second dialog appears centred as the first did.
- [ ] Probe overhead: front-end idle, 60 s, with `XERCES_BENCH=1` and
      `XERCES_BENCH_AUTO=menu:10:60`, against the same scene without
      `XERCES_BENCH` timed by PresentMon alone; 3 runs each in A-B-B-A order.
      The mean frame time and the 99th percentile must agree within run-to-run
      noise.
- [ ] Probe against PresentMon: one probe run with PresentMon recording
      `--process_name boiii.exe --qpc_time` over the same window. The probe's
      frame count (`perf.bench.written`, `<run>.frames.csv`) and PresentMon's
      present count for the game's swap chain agree within a frame or two at
      each end, and the dt series match frame by frame when joined on QPC
      (`t0_qpc` and `qpc_frequency` in `<run>.meta.json`).
- [x] (2026-11-05, 843ba747) In a match, `overlay_us` stays near 0 (mean 0.001
      µs: the idle skip), and the `cpu` column shows the render thread
      presenting from the P-cores only (logical 0, 1, 10-13, 22, 23). Front-end
      idle not measured.
- [x] (2026-11-05, 843ba747) `XERCES_BENCH=1` arms the probe
      (`perf.bench.armed`) and `XERCES_BENCH_AUTO=ingame:20:60` writes one run
      20 s after the player is in (`perf.bench.written`, the three files), in 16
      of 16 runs that reached the window.
- [ ] `xerces_bench_map zm_ccube` from the console logs `perf.load` with the
      time to the first main frame with the client active, near what the
      journal's `loading_map` to `in_game` shows.
- [ ] `XERCES_BENCH_OUT` under AppData or in the game folder logs
      `perf.bench.refused` and writes nothing.

### In-game frame times (2026-11-05)

`tools\dev\bench\game-bench.ps1 -Variant <name> -OutDir <session> -Live` runs
one measurement (with your go-ahead for the session):

- it starts the game through `xerces_cli plan apply` with the home
  `C:\Users\Ginld\XercesHome-bench`, so no launcher window runs during the
  window;
- it loads the map from the menu and, 3 s after the player is in, types
  `notarget` and `god` (cheats come from a session-only launcher dvar,
  `boiii_allowCheats 1`, which `config.cfg` never keeps);
- the probe records `XERCES_BENCH_AUTO=ingame:20:60`. Nothing is sent to the
  game during the window; one `GetForegroundWindow` every 2 s notes any other
  window in front;
- it then waits for `perf.bench.written`, takes a picture, disconnects and
  quits. One JSON line per run goes to `<session>\results.jsonl`.

`-Dvars` (session-only `+set`), `-FullScreenMode` (`boiii_players\config.ini`
edited for the run and put back byte for byte after it), `-Stack`, `-Affinity`
and `-Priority` set up a variant.

Session 2 (`C:\Users\Ginld\XercesDev\bench\20261105-fps2`): Cheese Cube, no mods
unless noted, your settings of that evening (100% render resolution, high mesh,
shadow, volumetric and GTAO quality, motion blur off), three runs per variant in
rotated order, 2 s of warm-up dropped. All 15 runs measured; the game's config
files were as before afterwards (only `com_maxclients` in `config.cfg` changed,
game state).

| Variant                             | FPS   | Median ms | p99 ms | 1% low | 0.1% low |
| ----------------------------------- | ----- | --------- | ------ | ------ | -------- |
| Windowed, uncapped (`w0`)           | 315.8 | 3.163     | 4.032  | 230.8  | 191.3    |
| Exclusive fullscreen (`f0`)         | 345.3 | 2.996     | 3.562  | 254.2  | 203.3    |
| Borderless (`b0`)                   | 312.3 | 3.163     | 4.042  | 226.0  | 182.8    |
| Windowed + D3 mod stack (`s0`)      | 301.0 | 3.336     | 4.254  | 213.7  | 173.4    |
| Windowed, `com_maxfps 240` (`w240`) | 239.1 | 4.443     | 6.050  | 158.1  | 140.6    |

Against `w0` (Welch, 3 runs a side):

- exclusive fullscreen: +9.4% FPS (p 0.024), p99 -11.6% (p < 0.001), 1% low
  +10.2% (p 0.003);
- borderless: no difference (its swap chain is still windowed; in 2 of 3 runs
  the game's console window, not the game, was in front, so those frames are
  flagged unfocused);
- the mod stack: -4.7% FPS (p 0.21), 1% low -7.4% (p 0.024), 0.1% low -9.4% (p
  0.038). Only 4 script errors fired in the match, and Present took longer
  (median 2.08 ms against 1.77), so the cost is GPU work, not scripts;
- `com_maxfps 240`: the cap holds (239.1 FPS) but pacing is much worse: p99
  +50%, 1% low -31.5%. The engine's limiter counts whole milliseconds (DESIGN.md
  12.4), so frames leave at 3.0, 3.5, 4.5 and 5.0 ms instead of 4.17 ms.

Uncapped, the game is GPU-bound at these settings: Present blocks a median 1.77
ms windowed (1.50 ms in exclusive fullscreen, where the desktop compositor does
not copy each frame). No hitch over 12 ms in any session 2 run.

The shake-out run (the settings before, 60% render resolution, the menu's
`com_maxfps 240`) showed the same limiter pattern: 230.7 FPS mean, main thread
4.0-5.0 ms, Present 0.07 ms (CPU-side limiter, not the GPU). Session 1 is not
usable: graphics settings were changed during it. The NVIDIA driver's 238 cap
noted in October does not apply to the game: uncapped runs reached 550-630 FPS
at the old settings.

Not covered: a heavier map, PresentMon (displayed frames, present mode, CPU and
GPU busy), more than 3 runs a side.

### Voice chat starts as push-to-talk (2026-11-05)

Built and unit-tested (`test_voice_default.cpp`), not run in the game yet. The
game keeps the choice in the player's profile (`snd_voicechat_open_mic`), so
Xerces sets push-to-talk only when the game has no profile yet
(`boiii_players\user_settings_<n>.cgp`), with one attempt from the first front
end's Lua (DESIGN.md 10.5). The talk key is B (`+talk` in the game's default
bindings).

- [ ] With `boiii_players` moved aside for the test (put it back after), so the
      game makes a new profile: the game log has `xerces.voice.default_due`,
      then `xerces.voice.push_to_talk` (not `xerces.voice.not_set`) once the
      front end has loaded; the game's voice options show Push to talk on; and
      `boiii_players\user_settings_0.cgp` exists after the game is closed. After
      a match, back in the front end, the log has no second line from Xerces
      about voice. After a restart the switch is still on and the log says
      `xerces.voice.kept`. With a second player in the lobby, they hear you only
      while you hold B.
- [ ] Then turn Push to talk off in the game and restart: it stays off (the
      player's choice is kept), and the log says `xerces.voice.kept`.
- [ ] On an install that already has a profile (your own: the 2026-11-04 test
      runs made `boiii_players\user_settings_0.cgp`): the log says
      `xerces.voice.kept`, and the voice options show the same Push to talk
      setting as before the start.

### Launcher (M3)

`tools\build.ps1 -Launcher` builds `build\bin\x64\Release\Xerces.exe` from the
third-party sources of the spike. Until the rename (DESIGN.md 7.7) it starts
`boiii.exe` from the same folder as the game role.

Checks without the game: always pass `--dry-launch` (the launch builds the plan
file and the command line, logs them and starts nothing; `XERCES_DRY_LAUNCH=1`
does the same for `Xerces.exe` and `xerces_cli plan apply`), and point
`XERCES_HOME` at a folder outside AppData (files created under AppData by
programs started from the Claude app land in a private copy):

```
$env:XERCES_HOME = 'C:\Users\Ginld\XercesHome'
build\bin\x64\Release\Xerces.exe --dry-launch --skip-boot --view home --script steps.txt --seconds 20
```

`tools\dev\launcher\check-screens.ps1 -Out <folder>` opens every screen that
way, captures it and lists any RmlUi warning (also with `-Theme light`,
`-Theme contrast` or `-Language pseudo`). A script plays timed steps, the first
at 1 s: `+500 key down`, `+300 pad a`, `+200 view mods`,
`+100 click launch-vanilla`, `+100 type text`, `+100 focus search`,
`+100 shot C:\path\screen.png` (a PNG of the next frame), `+100 quit`. The log
is `<home>\logs\<date>-<time>-launcher-<pid>.jsonl`.

Run on 2026-11-05 by Claude on your PC while you were away (dry launches only):

- [x] Native paint 24 to 38 ms, first RmlUi frame 211 to 260 ms warm.
- [x] The game was found and its build called supported; the catalog read 17
      Workshop items. The first scan of the 59 GB Workshop folder took 195 s;
      with the hash cache (`<home>\cache\hashes.txt`) a rescan takes 0.09 s.
      (The first scan took 10.4 s after the launcher speed work below.)
- [x] Launch modded with the D3 stack builds
      `-launch -nointro -xerces-plan <id> +set fs_game 1944930083 +set fs_extramods 3171632315,3656779213`,
      the command D3 ran in the game, and writes the plan file.
- [x] A journal that ends in `in_game` with mods makes Home offer safe mode; a
      culprit search went from test 1 to test 2 across two launcher runs, the
      result read from a journal written for the test.
- [x] A second `Xerces.exe` on the same home brings the first forward and exits.
- [x] Two mods that declare each other as conflicts (`fake-game\mods\conflict_a`
      and `conflict_b`) block the plan; Mods' "Allow this conflict" turns the
      problem into the warning "Allowed by you", the plan then has no problems,
      and `config\launcher.json` keeps the pair.
- [x] (launcher/fix2-redo) A pad reaches About from Home (Down past the screen's
      end), Settings (Up from the tabs) and Diagnostics (it scrolls to its end
      first); the review also reached it from Mods, Packs and Servers (B, Up,
      A). Every screen in English and in the pseudo-locale has no RmlUi warning,
      and long panel notes wrap under their titles.
- [x] (launcher/fix2-redo) Home's culprit box on a fake game with dependencies
      and maps. Each state was written into `config\culprit.json` by hand, one
      the search reaches (traced step by step, as `test_culprit.cpp` runs the
      real search); the search itself did not run across launches, because a dry
      launch records no test result. A dependency that one enabled mod loads, or
      two, is named with a sentence for one or for several, and the button turns
      off only those mods. One that the map and an enabled mod both need gets no
      button: the box says to pick another map and turn the mod off in Mods, and
      a click on the hidden button changes nothing. A dependency only the map
      needs, two that only load with the map, a crash with a map where no test
      ran fine (the map named as a possible cause), and a test of the map's
      dependency alone each say so. A save that fails (`launcher.json`
      read-only) shows its reason translated in the pseudo-locale.
- [ ] The window by hand: drag by the header, snap, resize from the edges,
      minimize, maximize, close; at 150 percent.
- [ ] A physical controller: A, B, Y, LB/RB and the D-pad on every screen.
- [ ] Q and E with Num Lock on switch tabs (fixed 2026-11-05 after review;
      scripted keys never carry lock flags, so only a real keyboard shows it).

Screens, each built and checked by scripted runs (dry launches) on 2026-11-05;
what still needs you:

- [ ] Mods: enable a few, reorder with Ctrl+Up/Down and with the mouse (drag),
      Ctrl+Z, search; "Open in Steam" opens the item in Steam.
- [ ] Settings: the player name shows in the game (written to
      `boiii_players\properties.json`, where the client reads it); FPS limit,
      field of view, V-Sync and the FPS counter take effect in the game; display
      mode (windowed, borderless); a value the game had saved in config.cfg no
      longer wins over Xerces' (launch_dvars.cpp), and config.cfg still holds
      the game's own field of view and FPS limit after the session, so "Game's
      setting" brings them back (`xerces.dvars.session` in the game log).
- [ ] Binds and asset pools (from Settings): a custom bind works in the game;
      with larger asset pools off, a big mod that needs them fails as it did
      upstream. Checked on a copy of the folder only, never the real files.
- [ ] Servers: join a self-hosted server by address (needs a server).
- [ ] Packs: export a pack, apply it on another profile or PC, and get the same
      plan id; a share code round trip. A pack that allows a conflict or sets a
      mod setting brings those decisions along (Mods shows the conflict as
      allowed). Applying writes a config backup to `<home>\backups` first.
- [ ] First run: on a PC with BOIII data, the import offers the name and the
      favourite servers (`boiii_players`); nothing under
      `%LOCALAPPDATA%\boiii\user` is opened.
- [ ] Diagnostics: "Verify with Steam" opens Steam's file check; Copy report
      pastes a report with `%USERPROFILE%` instead of your folder.

### Launcher speed (2026-11-05)

DESIGN.md 12.6 has the numbers and what changed. Nothing here starts the game:
`Xerces.exe` runs with `--dry-launch` only, and Workshop files are only read.

- `tools\dev\bench\launcher-bench.ps1 -Runs 20 -HomeDir C:\Users\Ginld\XercesHome-bench`
  starts
  `Xerces.exe --home <home> --dry-launch --skip-boot --view home --seconds 2` 20
  times, then once without `--skip-boot` for the boot screen, and prints the
  median, p90, mean and a 95% interval of every startup mark and measured event.
  `-Exe` runs another build (keep the old one as `Xerces-old.exe` next to the
  new one for before and after; it finds `data\` the same way), and `-Label`
  names the JSON saved in `<home>\bench`. Seed the home first by copying another
  home's `cache\hashes.txt` and `config\launcher.json` into it (never write into
  the other home). `-FreshScan` times one scan with no cache in `<home>-fresh`:
  it reads the whole Workshop folder (63 GB here), so it never runs by default
  and never right before load benchmarks.
- `build\bin\x64\Release\xerces_tests.exe --bench-hash [--threads 1,2,4,6] <file>...`
  on a few large Workshop files: the portable code and CNG on 1 GiB in memory,
  the disk unbuffered (1 to 6 files at once), a buffered read twice (the second
  from the file cache), then the files hashed by the scan's code, by the
  portable code, and by CNG 1 to 6 at once. The files are opened for reading
  only. For before and after, build the old `xerces_tests.exe` with the
  benchmark first (the "before" numbers in DESIGN.md 12.6 are
  `try_file_sha256_hex` at fcfa9f01).
- `XERCES_TEST_V1_CACHE=<a version 1 hashes.txt>` with
  `xerces_tests "real version 1"` migrates a copy of a real version 1 cache and
  checks that every line survives; the file named is never written.
- The launcher's log events for this: `launcher.view.loaded` (ms per screen
  document), `launcher.view.shown` (ms spent in `show`),
  `launcher.services.resolve` (debug, ms per plan resolve),
  `launcher.refresh.timing` (cache load, detection, pre-flight, scan and cache
  save, in ms), `launcher.catalog.scanned` (with per-file and overall MB/s, the
  slowest file and the threads), and `launcher.activity` every minute and at
  exit (frames presented per minute in front, behind, minimized and while a game
  runs, and the resolve totals). With `--seconds` the loop never waits, so the
  benchmark's frame counts are an upper bound, not what a player's launcher
  draws.

Run on 2026-11-05 by Claude (no game running, dry launches only):

- [x] 20 dry starts before and after: the game detected 322 ms after start
      before, 54 ms after (medians); the catalog ready at 409 and 214 ms; the
      first frame at 224 ms both times. Through the boot screen Home shows at
      675 and 682 ms (the 650 ms floor).
- [x] One scan with no cache: 63.3 GB in 10.4 s (6.1 GB/s, six files at once;
      the 9.0 GB file took 8.7 s), against 195 s before. Every file's hash
      equals the one cached in `XercesHome-maps`, and the 17 content hashes and
      the plan id (`sha256:7fb8e51e...`) are unchanged.
- [x] `C:\Users\Ginld\XercesHome\cache\hashes.txt` (version 1) migrates line for
      line (on a copy).
- [x] After review: `XERCES_HASH_CACHE` naming a version 2 file whose one line
      ends in `caf\xC3.ff` (cut inside a character) no longer fails
      `xerces_cli mods list --home C:\Users\Ginld\XercesHome-bench` (exit 0, as
      with a valid "é"), and `Xerces.exe --dry-launch` on that home reads its 17
      items instead of ending in `std::terminate`. Its scan event reports 686
      hits, without BlackOps3.exe.

### Conflict system (M2)

- [ ] `xerces_cli test run both-apply` with the loose pair: the merged table
      shows both rows and both edits, and `xerces_test_hooks` reads
      `fixture.alpha;fixture.beta;probe;`.
- [ ] The same with the fastfile pair built with the BO3 Mod Tools.
- [ ] The pre-launch conflict report matches what happens in the game for two
      legacy mods that replace the same script.

## Performance

How to get numbers (DESIGN.md 12.2 and 12.3; the plan is
`C:\Users\Ginld\XercesDev\perf-plan-20261105.md`). The `bench` commands below
read files that already exist. They start nothing and write nothing.

### Game starts and map loads, from logs

```
xerces_cli bench loads --logs C:\Users\Ginld\XercesHome-maps\logs --console C:\Users\Ginld\XercesDev\map-checks [--json]
```

`--logs` takes a home's `logs` folder and can be repeated. `--home <dir>` adds
`<dir>\logs`, and with neither `--logs` nor `--console` the default home is
used. `--console` takes a map-check folder (one run per subfolder, with
`results.jsonl` beside them).

- **Game start:** `launch.started` (matched to the game log by pid) to the
  game's first `loader.phase frontend`, the first main frame. Xerces.exe logs
  `launcher.launch.started` and `xerces_cli` logs `cli.launch.started`; each
  launch says which (`launched_by`), and the summary keeps them apart
  (`startup.game_start_to_frontend_s.launcher` and `.cli`). The game log's first
  line comes 14 to 16 s after the launch, when Xerces' code first runs.
  `first_log_to_frontend_s` measures from that line, for logs with no launcher
  log beside them.
- **Map loads (map-check runs):**
  - "to level" is the console's `map` command to the engine being in the level.
  - "to player" is the command to the journal's `in_game`: the player is in the
    level (`loader.map.ready`).
  - "level->player" is the game log's `loading_map` to `loader.map.ready`.
  - The logs do not timestamp the command yet, so the first two come from
    map-check's stopwatch, which is not the command's time:
    - it starts after `console-command.ps1`'s 1.5 s echo wait, so about 1.5 s
      after the game ran the command;
    - it stops when map-check sees the journal's `in_game`. map-check polls
      about once a second, and with `-SkipCinematic` a poll can first hold Space
      for 1.5 s. The game itself writes each phase up to 1 s late (it checks
      once a second).
    - So each figure is off by roughly -1.5 to +2.5 s: low when the polls are
      quick, high when they are slow. Compare them only with other stopwatch
      figures. A load the game times itself (perf plan H3's `xerces_bench_map`)
      will not match them.
  - "To player" includes any intro or wait before the player is in. With
    `-SkipCinematic`, map-check presses keys every 6 s from 15 s on. The one
    Shadows of Evil run that did not skip waited 214 s for its intro.
  - Journals from before 2026-11-05 11:25 have no `loading_map`. Their `in_game`
    came as soon as the engine was in the level, so those runs give "to level"
    only (`"journal": "old"`). A log that never reached a level (the game ended
    right after the map command) cannot show which kind it is:
    `"journal": "unknown"`.
  - A run that failed while loading still counts, from its map-check record.
- **Zones:** the console's own `FastFileLoad ... in N ms` times, summed by group
  (map, main mod, extra mods, front end, stock) up to the map's own zone. Zones
  after it, such as a drop back to the menu, are `after_map_zone`. Zone loads
  can overlap, so the sum is not wall time.
- A home's `logs` folder keeps only its last 20 sessions. Save the JSON, and a
  copy of the logs, whenever a set of runs matters.

### Launcher starts, from logs

```
xerces_cli bench launcher --logs C:\Users\Ginld\XercesHome-maps\logs --logs C:\Users\Ginld\XercesHome-m1\logs [--json]
```

This gives the median, p90, mean and 95% CI of each `launcher.startup.step` mark
(ms from the launcher's start), the time to the session's first
`launcher.game.detected` and first `launcher.catalog.scanned`, and the Workshop
scans. Rescan, a new game folder and other refreshes log both events again;
those count as `later_detections` and `later_scans`, not as startup samples. A
scan is warm when the hash cache had every file and cold when files were hashed
(GB/s per scan). Warm and cold take every scan, startup or later
(`scans.list[].startup` says which).

### Frame times

```
xerces_cli bench analyze <run.csv|folder>... [--source probe|presentmon] [--warmup-s 10] [--json]
xerces_cli bench compare --a <folder|csv> --b <folder|csv> [--source probe|presentmon] [--margin-mean 1] [--margin-p99 3] [--warmup-s 10] [--json]
```

Each CSV is one run, in one of two formats:

- the probe's frames CSV (`i,t_ms,dt_ms,present_ms,overlay_us,cpu,flags`),
  written into `XERCES_BENCH_OUT` (`game-bench.ps1` gives each variant its own
  folder; see "In-game frame times").
- PresentMon's CSV, with 1.x, 2.0 or 2.3+ column names. The process and swap
  chain with the most rows are kept.

A CSV named on the command line is one run (a `*.main.csv` is refused). A folder
gives the runs in it and in its subfolders, one level down, which fits the
plan's layout of one folder per run (`bench\<run>\` with `<run>.frames.csv`,
`<run>.main.csv` and PresentMon's `presentmon.csv`):

- each `*.frames.csv` is a probe run;
- each other CSV with PresentMon's columns is a PresentMon run;
- `*.main.csv` (the probe's main-thread cadence) and other CSVs are left out.

The probe and PresentMon time the same frames, so one command takes one source.
When the runs found hold both, `--source probe` or `--source presentmon` picks
one; without it the command stops rather than count a run twice.

`--warmup-s` drops the frames before that many seconds from the first frame. The
counts (`rows`, `rows_skipped`, `not_displayed`, `present_modes`,
`flagged_frames`, `frames_per_cpu`) are then taken over the frames kept, and
`warmup_rows_dropped` and `warmup_frames_dropped` say what went. PresentMon's
`other_rows` (other processes and swap chains) is for the whole capture.

The numbers, as `src/xerces_core/perf_stats.hpp` defines them:

- mean frame time (total time / frames) and average FPS;
- median, p95, p99, p99.9 and max (percentiles interpolate between the closest
  ranks);
- 1% and 0.1% lows, each given two ways and labelled: 1000 / the mean of the
  worst frames, and 1000 / p99 or p99.9;
- hitches, frames over 50 ms and over 100 ms, and stutter time (the time spent
  in hitch frames / the total). A hitch is a frame longer than both 2x the
  rolling median of 31 frames and that median + 4 ms;
- drift between the two halves of the run. Above 2% the run was not steady;
- for PresentMon, the mean CPU-busy and GPU-busy times. They tell whether the
  game is CPU-bound or GPU-bound.

`compare` treats each run as one sample, because the frames inside a run are not
independent:

- 95% t intervals for each side;
- Welch's B - A, with its interval and p;
- TOST equivalence: B is equivalent when the 90% interval of B - A lies inside
  ±margin, a percentage of A's mean. The defaults are 1% on mean frame time and
  3% on p99, the proposed M7 margins.
- pooled 1% and 0.1% lows from a cluster bootstrap (whole runs resampled 2000
  times, seed 20261105). These describe the data; they are not a test.

For a comparison, take 5 or 6 runs a side of the same scene, in A-B-B-A order.

PresentMon 2.4.1 is installed at
`C:\Program Files\Intel\PresentMon\PresentMonConsoleApplication\PresentMon-2.4.1-x64.exe`.
Capturing needs an elevated terminal, and each game session needs your go-ahead.
Check the flags with `--help` first; the plan's capture is:

```
PresentMon-2.4.1-x64.exe --process_name boiii.exe --output_file <run>\presentmon.csv --timed 80 --terminate_after_timed
```

With `--qpc_time` the start times are QPC ticks, which `bench` does not convert.
It then sums the frame times instead (for `--warmup-s`).

- [x] (2026-11-05) Probe frame times recorded in the game: 16 runs of 60 s (see
      "In-game frame times"). No PresentMon capture yet.

### First baseline (2026-11-05, existing logs only)

The baseline is in `C:\Users\Ginld\XercesDev\bench\baseline-20261105\`:
`loads.json` and `launcher.json` (both homes), the per-home files, the same as
text, `commands.txt`, and copies of the logs read (`inputs\`). It was taken at
19:14 UTC.

- [x] Game start to the first main frame:
  - D3 stack, the six newest launches on XercesHome-maps (12:52 to 13:03 local,
    three of them crashing later at the map): 36.2 s mean (95% CI 34.8 to 37.7,
    range 34.5 to 37.9);
  - XercesHome-m1, through Xerces.exe: 34.4 s (range 33.0 to 35.4, n 5). One
    more launch there came from `xerces_cli` (33.8 s); the first baseline files
    counted it with the others (34.3 s, n 6).
  - The game log starts 20.0 s before the first main frame (n 53): 18.9 s with
    no mods, 20.4 s with the D3 stack.
- [x] Startup zone loads, summed over 48 consoles that hold the whole start:
      14.9 s in total. The front end takes 3.5 s, the stock zones 10.3 s, and
      the extra mods 1.1 s when stacked.
- [x] Map loads, 53 attempts, 32 reaching the match:
  - "To level" and "to player" are map-check's stopwatch. It starts 1.5 s after
    the map command and stops up to about 2.5 s after the event, so each figure
    is off by roughly -1.5 to +2.5 s (above, Map loads).
  - With mods: 4.5 to 7.5 s to the level on the stopwatch, then 16 to 41 s more
    before the player is in (from the game log, ±1 s).
  - Cheese Cube with the stack: 7.1 s to the level (n 5) and 27.0 s to the
    player (n 4) on the stopwatch; 20.3 s from level to player.
  - With no main mod, every custom-map load first reloads the front end's zones
    inside the usermaps ModLoad (3.4 to 3.6 s, 7 loads). The stock map zm_zod
    does not.
  - The baseline's `loads.json` predates two fixes. It labels the four runs
    whose game ended before the level `"journal": "old"`, though three of them
    (12:52 to 12:55) had the new journal; `bench loads` now calls all four
    `"unknown"`. It also gives game starts by variant only, with Xerces.exe and
    `xerces_cli` launches together.
- [x] Launcher, 26 starts, all warm:
  - first frame at 220 ms median (p90 239 ms);
  - game detected at 324 ms;
  - catalog scanned at 410 ms;
  - every scan was warm (0.08 s). No cold scan is in these logs, so the 195 s
    first scan above is not re-measured.
