# Testing Xerces

What runs by itself, and the checklist for what only the real game on your PC
can show (docs/DESIGN.md 15.3 and 15.4). The checklist grows every milestone.

## Automated

| What                                                                                                                                                                           | Command                                                        | Needs                         |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------- | ----------------------------- |
| Core unit tests: manifests, SemVer, the planner and its determinism, merges, conflict rules, hook chains, packs and share codes, logs, the loose both-apply pair               | `tools\build.ps1 -Tests`                                       | LLVM 23, Ninja, premake 5.0.0 |
| CLI smoke: plan, conflicts and pack round trip on the fixtures                                                                                                                 | `tools\build.ps1 -Tests`                                       | the same                      |
| CLI smoke on a home: mods enable, disable, order and info, `plan apply --dry-launch`, `packs apply`, `logs tail`, `sig check` on a fake game folder                            | `tools\build.ps1 -Tests` (`tools\ci\cli_smoke.ps1`)            | the same                      |
| Benchmark: frame statistics, Welch, TOST, the bootstrap, the probe/PresentMon/console/log readers; `bench` on small logs and CSVs                                              | `tools\build.ps1 -Tests`                                       | the same                      |
| Signature registry against a stored dump (`compat/signatures.json`)                                                                                                            | `xerces_cli sig check --dump C:\Users\Ginld\XercesDev\dumps`   | a dump on this PC             |
| Pre-flight check of the installed game against `compat/builds.json`                                                                                                            | `xerces_cli game check`                                        | the game on this PC           |
| Hashing micro-benchmark: disk read speed apart from hashing speed, the portable code against CNG, files hashed 1 to 6 at once (reads only)                                     | `xerces_tests --bench-hash <file>...`                          | large files (Workshop)        |
| Launcher start times: 20 dry starts and one through the boot screen, summarized from the launcher log                                                                          | `tools\dev\bench\launcher-bench.ps1`                           | a seeded home                 |
| Crash reports: the contract's fields (a golden report), redaction, the outbox's limits, the setting off sending nothing (a fake transport), gzip, the crashing module's origin | `xerces_tests crash`                                           | the same                      |
| Backend: auth, rate limits, privacy, migrations                                                                                                                                | `cd backend` then `pytest -q`                                  | Python 3.12+                  |
| Formatting: prettier, clang-format, stylua                                                                                                                                     | `yarn run check`, `clang-format --dry-run`, `stylua --check .` | Node, LLVM, StyLua            |

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

| File                      | Source                                       | Bytes      | SHA-256                                                            |
| ------------------------- | -------------------------------------------- | ---------- | ------------------------------------------------------------------ |
| `RmlUi-6.3.zip`           | github.com/mikke89/RmlUi, tag 6.3 (MIT)      | 4,513,038  | `bfb340660e4bce38d4211cffc836cfbe1be676aa36ef9cc48e7df3855ee74fe3` |
| `freetype-2.14.3.tar.xz`  | download.savannah.gnu.org (FTL or GPLv2)     | 2,670,220  | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` |
| `InterTight-wght.ttf`     | github.com/google/fonts, `ofl/intertight`    | 581,588    | `b81b73dcb64df3c230cabade7df6c5773bf863233f24c9ee51087519f1f88b6f` |
| `IBMPlexMono-Regular.ttf` | github.com/google/fonts, `ofl/ibmplexmono`   | 135,580    | `6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46` |
| `IBMPlexMono-Medium.ttf`  | github.com/google/fonts, `ofl/ibmplexmono`   | 136,704    | `a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755` |
| `harfbuzz-15.0.0.tar.xz`  | github.com/harfbuzz/harfbuzz, release 15.0.0 | 20,396,372 | `968536e9abd782f07f7954044c0c4020199fcdd39b41d221ec42d2a3be4a44df` |

The fonts and their OFL texts are in `data/xerces/fonts/` (unmodified; the
variable font's file name drops the `[wght]` suffix). HarfBuzz ("Old MIT"
licence) was downloaded on 2026-11-05 with your approval and unpacked as
`third_party\harfbuzz\harfbuzz-15.0.0`; it is optional and only the launcher
uses it (DESIGN.md 7.1).

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
- [ ] Inviting a friend says the feature is off in this build. `friends_open` is
      the lobby's DIRECT CONNECT button since 2026-11-05 (Joining another
      tester's game, below).
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
- [ ] Out of memory (DESIGN.md 5.6, feature `oom_report`; built 2026-11-05), the
      rest of this run: the BACKROOMZ part passed on 24bd2f9b (the item above).
      Still to do: `zm_supermario64` the same way, then a map that fits with
      DS4C, and whether the next launcher start offers safe mode. The run: DS4C
      alone, then `map zm_backrooms`, through
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
      mods every map played, RETURN TO HOGWARTZ only on the second try: its
      first load without mods (11:23, `results.jsonl` row 14) ended the game 4.2
      s in with no dump and no report (`crash_load`); the next (11:25) played.
      Not explained.
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
- [x] (2026-11-05 15:01-15:10, `results.jsonl` rows 57-61) Larger zone memory
      ran in the game five times: DS4C alone on BACKROOMZ and on Super Mario 64,
      then the stack on BACKROOMZ, Super Mario 64 and Cheese Cube. Every run
      logged `xerces.db_pool.installed` and `xerces.db_pool` (main pool 4096
      MiB, the first `main` call 0x80000000 given 0x100000000, page budget 8192
      MiB, read back 4096 MiB), loaded the map (27-47 s), played 30 s and went
      back to the menu, with no `loader.exit.unexpected` and no dump. The main
      pool held 2459 MiB in BACKROOMZ and 2110-2111 MiB in Super Mario 64, more
      than the stock 2048; Cheese Cube stayed within it (1572 MiB). What was
      checked, as planned before the runs (DS4C alone on BACKROOMZ:
      `tools\dev\session\map-check.ps1 -Map zm_backrooms -Stack 3171632315 -ExtraFlags -xerces-db-pool -Live`),
      in the game log (`<home>\logs\<date>-<time>-game-<pid>.jsonl`, copied to
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
      `loader.terminate` before the fail-fast. None has been logged since, but
      the game has failed since: in the first frame-time session (In-game frame
      times, below), a run with no mods was gone 17 s after the menu with no
      report and no exit record (17:04, `w240`, the same signs as above), and at
      17:13 Cheese Cube with the stack crashed while starting (`s240`): an
      access violation in the game's code under a scheduler callback, reading
      0xFFFFFFFFFFFFFFFF at BlackOps3.exe + 0x14221A349 (report and dump in
      `C:\Users\Ginld\XercesDev\dumps\bench-20261105`). The next runs of both
      variants were measured. Neither is explained.

### Menu crash at the first main frame (2026-11-05)

- [x] The 17:13 crash (dump
      `C:\Users\Ginld\XercesDev\dumps\bench-20261105\crash-2026-11-05-17-13-04.dmp`;
      the frame-time bench's run 5 of 15, `s240`, the D3 stack on Cheese Cube,
      `C:\Users\Ginld\XercesDev\bench\20261105-fps\runs\20261105-171239-s240`).
      The cause was found in the dump and confirmed by a second, independent
      read of it, before the fix. Upstream's `IN_SetActiveDevice_stub`
      (`client_patches.cpp`, from upstream f8f7b735, "Fix a crash when switching
      between controller and keyboard in the menus") runs a call made off the
      main thread in the next main frame (`scheduler::once`). A mouse-button
      change on the game window while the game loads reaches `IN_MouseEvent` on
      the window thread while `primaryLocalClientNum` is still -1. The deferred
      call then ran in the first main frame with local client -1, and the engine
      indexed its per-local-client tables with it: the fault is in
      `Storage_IsFileReady` with controller -1. The game log ends at
      `loader.plan.applied` (phase `frontend`, the first main frame), with
      nothing from Xerces after it; the bench script logged "menu after 25.4 s",
      then the crash dialog (`crash_load`).
- [x] The same log signature in two map-check runs on `XercesHome-maps`, the D3
      stack, no dumps taken: 12:52 (`zm_mw2rust`) and 12:55 (`zm_ccube`). Both
      are "the game ended after 0 s of loading" in
      `C:\Users\Ginld\XercesDev\map-checks\results.jsonl`, with the journal left
      in `frontend` and the game log ending at `loader.plan.applied`. Their
      cause is inferred from that signature, not read from a dump. The 12:53 run
      (`zm_mw2rust`, "the game ended after 9.5 s of loading") is not counted
      with them, although its game log
      (`C:\Users\Ginld\XercesDev\map-checks\20261105-125335-zm_mw2rust-stack3\20261105-185355-game-30488.jsonl`)
      and its journal
      (`C:\Users\Ginld\XercesHome-maps\logs\journal.json.20261105-125551`) end
      the same way, at `loader.plan.applied` with the phase `frontend`: only the
      script's load timing differs, so it may be the same crash.
- [x] Fix (`xerces_core/active_device`): the hook checks the local client where
      it calls the original, on the direct path and in the deferred call, and
      drops a call for a local client outside 0 to `LOCAL_CLIENT_COUNT` - 1. The
      first drop logs `xerces.input.active_device_dropped` at info, with the
      local client, the device and whether the call was deferred; later drops
      are not logged. No engine address is added. `game/symbols/in.hpp` names
      the parameter a local client number (`LocalClientNum_t`, the same 32-bit
      argument as the `ControllerIndex_t` it had). Unit tests in
      `test_active_device.cpp`. Not run in the game.
- [ ] In the game, with your go-ahead, on a modded launch: click on the game
      window and move the mouse over it while the game loads, before the menu.
      The game reaches the menu; the log may hold one
      `xerces.input.active_device_dropped` with `local_client` -1 and `deferred`
      true. In the menu, switching between a controller and the keyboard still
      changes the button prompts (upstream's fix still works). Then rerun the
      frame-time bench's `s240` runs and map-check on Cheese Cube with the
      stack, and note whether a run ends in `crash_load` with the game log
      ending at `loader.plan.applied` (phase `frontend`), whatever load time the
      script gives: "the game ended after 0 s of loading" as at 12:52 and 12:55,
      or a few seconds as at 12:53.

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
- [ ] Flip-model presentation (`-xerces-flip-model`, DESIGN.md 12.4 item 4;
      built 2026-11-05, never run in the game). The launcher side was checked in
      a dry run only: Settings, Display, "Flip-model presentation
      (experimental)" puts `-xerces-flip-model` into `launcher.json`'s
      `extra_flags` and takes it out again; the DXGI side was checked offline
      with a scratch program (DESIGN.md 12.4 item 4). One session, with your
      go-ahead, Cheese Cube, uncapped, 3 runs a side in A-B-B-A order:
      `tools\dev\bench\game-bench.ps1 -Variant flip -ExtraFlags -xerces-flip-model -Dvars @{com_maxfps='0'} -OutDir C:\Users\Ginld\XercesDev\bench\<session> -Live`
      against the same without `-ExtraFlags` (`-Variant w0`), then both again
      with `-FullScreenMode 2` (borderless). In the game log:
  - `xerces.flip_model.armed` with `armed_by` `post_unpack` (the overlay's
    Present hook came first, as expected), then `xerces.flip_model` at info:
    chain 1, asked `discard` with flags 0x2, made `flip_discard` with 0x802 (0x2
    if the PC reports no tearing support). A warning instead names the reason,
    and the run proves nothing about flip model. `hooks_not_ready` means the
    overlay's Present detour (and so ResizeBuffers) was still missing when the
    engine made its chain; its `error` says which. `hook_failed` names the
    detour that could not be installed.
  - `perf.swapchain`:
    `flip_discard, 2 buffers, 3840x2160, windowed, flip model: applied` with
    `engine_swap_effect` 0.
  - No `xerces.flip_model.resize_failed` or `xerces.flip_model.tearing_refused`.
  - On screen: the menu, the loading screen and the level draw and keep updating
    (no black or frozen picture) windowed, borderless and in exclusive
    fullscreen (`FullScreenMode 1`: the chain goes fullscreen through
    `SetFullscreenState`); a resolution or window size change keeps drawing
    (`ResizeBuffers`); Alt+Tab out and back works; the download overlay's dialog
    still shows and takes clicks (join a server whose map is missing).
  - A second chain: make the engine create its swap chain again. Which change
    does that rather than `ResizeBuffers` is not known yet (open question in
    docs/research/upstream-render.md); try the backbuffer count
    (`r_backBufferCount`), the display mode and the monitor, and `vid_restart`
    if the console takes it, watching the log. A second `xerces.flip_model` line
    with `chain` 2 at info shows one, and the picture keeps drawing. A warning
    or a black screen there means the new chain was made before the old flip
    chain was gone; note which change did it. If nothing makes a second chain,
    say so here.
  - If a modded launch with it crashes or is ended in Task Manager: the journal
    has `"flip_model": true`, the safe-mode message names flip-model
    presentation, and "Find the culprit" first runs the same mods without it
    (checked in `test_m1_runtime.cpp` and `test_launcher_shared.cpp` only).
  - Numbers: `xerces_cli bench compare --a <w0> --b <flip>` on the probe CSVs.
    The goal is windowed and borderless close to exclusive fullscreen's 345 FPS
    and its p99 and 1% low (DESIGN.md 12.4 item 4). The flags column has 32 on
    windowed and borderless frames with V-Sync off, and never with V-Sync on. If
    you run PresentMon, its PresentMode column should read a flip mode (composed
    flip windowed, independent flip borderless) instead of a copy.

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

### Beta package (DESIGN.md 16.1)

`tools\release.ps1` refuses to build while `notices.py --licences` finds code
GPL-3.0 cannot take in (`src/common/str.hpp` until its rewrite merges) or while
Settings lacks the crash report controls `README.txt` describes (until
`launcher/crash-client` merges). Both are merged in the beta candidate (below),
where the gates pass. Not checked yet, all with the game (ask first), from an
unzipped package outside the repository:

- [ ] The game reads the package's `data\` (`XERCES_DATA_PATH`): the Xerces
      menus and `ui_scripts` load with no `%LOCALAPPDATA%\boiii\data`.
- [ ] The menu watermark and the console title say
      `Xerces <version> (<commit>)`, the game window's title starts with
      "Xerces", the splash in the taskbar is "Xerces", and the in-game console's
      input line starts with `Xerces> `.
- [ ] A crash leaves `xerces-crash-<time>.zip` in
      `%LOCALAPPDATA%\boiii\minidumps` with `info.txt` headed "Xerces Crash
      Dump" and its `Version:` line.
- [ ] The first host or join shows the firewall prompt for "BOIII"
      (`KNOWN_ISSUES.md`).

### Beta candidate (launcher/integrate-2, 2026-11-05)

`xerces-main` 0212339c with `launcher/str-rewrite`, `tls-rewrite`, `harfbuzz`,
`crash-client`, `beta-package`, `join-password` (with `join-fixes`) and
`voice-ptt` merged, then the cross-branch fixes (DESIGN.md 18). Run by Claude
with dry launches only, homes outside AppData
(`C:\Users\Ginld\XercesHome-agent-integrate2`, `-notice`, `-cli`, `-pkg`),
scripts in `C:\Users\Ginld\XercesDev\scratch\integrate2`; the game was never
started and nothing was sent to the Railway service.

- [x] `xerces_tests`: 540 tests pass; `cli_smoke.ps1` passes; every lint and
      self-test (`check_fenced`, `check_hosts`, `check_contrast`,
      `check_licences`, `check_microsoft_notices`, `check_switches`,
      `notices.py --self-test`, `--check` and `--licences`, `tokens.py --check`,
      `extract_strings.py --check`); no fenced function, line or file differs
      from 0212339c.
- [x] `check-screens.ps1` (dark, light, pseudo): every screen captured, no RmlUi
      warning or error; Settings > Privacy shows the crash report switch,
      "Delete my crash reports", the GitHub link and the voice line.
      `check-idle.ps1`: five of six cases pass. Corrected by the review: "a game
      running, launcher behind" fails on this PC, 2.6 to 3.0 passes a second
      against a limit of 2 (0 frames), and fails the same way with `xerces-main`
      0212339c's own build (2.6 a second, 2026-11-06), so it is not from the
      merges. When launcher-idle was merged the case gave 1.2 to 1.8 (Launcher
      speed, below). What wakes the loop the extra times behind a running game
      is not found yet.
- [x] The MinHook speed check (`test_minhook_threads.cpp`) run as twelve copies
      at once, three times: 36 of 36 passed.
- [x] Crash report notices from the worker (`crash-wake-idle.ps1`): with 400
      stand-in pending notes the start-up job ends while the loop is idle behind
      other windows. A build without the wake took the result 10, 83, 61 and 65
      ms later; the merged build 0, 0, 0.1 and 0 ms (`launcher.crash.event`).
      "Delete my crash reports" in a dry launcher shows "This launcher runs with
      --dry-launch ..." 19 to 21 ms after the confirming click.
- [x] `xerces_cli crash list` and `crash send` against a stand-in backend on
      127.0.0.1 (`crash_stub.py`, `XERCES_CRASH_URL`): two stand-in pending
      crashes became reports, went out gzipped with the install id and
      `User-Agent: Xerces/0.1.0-beta.1`, came back 201, and left the outbox
      empty; the reports' `xerces.commit` and `dirty` came from
      `xerces_version.h`.
- [x] `tools\release.ps1` on the clean committed tree passed every gate (it
      warned that HEAD is not on origin) and wrote
      `dist\xerces-0.1.0-beta.1.zip` (236 entries, `/` in every name),
      `SHA256SUMS-0.1.0-beta.1.txt` and the symbols zip. `README.txt`,
      `KNOWN_ISSUES.txt`, `SOURCE.txt` and `THIRD_PARTY_NOTICES.txt` are UTF-8
      without a byte-order mark, with CRLF line ends; the notices have 32
      entries, HarfBuzz's with `COPYING`, `src/ms-use/COPYING` and the Unicode
      License V3; `data\xerces\licenses` is gone from the package.
- [x] The zip unzipped in the scratch folder, its `Xerces.exe --dry-launch` on a
      fresh home: `launcher.start` names the package's `data` as `client_data`,
      HarfBuzz runs, the first scan read 17 items, and Launch vanilla logged
      `launcher.launch.dry` with `xerces_data_path` set to the package's `data`
      folder. About shows the BETA badge, the commit's source link and the
      `THIRD_PARTY_NOTICES.txt` next to the program; no RmlUi warning or error.
- [ ] Everything with the game or a second PC: the two-PC join test
      (`docs/TWO_PC_TEST.md`), the crash report and push-to-talk items above and
      below, and the beta package items above.

Fixes from the two reviews of the candidate (2026-11-06), run by Claude with dry
launches only (homes `C:\Users\Ginld\XercesHome-agent-integrate2`, `-probe`,
`-firstrun-en`, `-firstrun-pseudo`); scripts and captures in
`C:\Users\Ginld\XercesDev\scratch\integrate2\fix`. The game was not started and
nothing was sent to the Railway service.

- [x] `xerces_tests`: 541 tests pass, one new
      (`crash outbox: a server without     crash routes is sent nothing`);
      `cli_smoke.ps1` passes; every lint and self-test as above,
      `notices.py --self-test` with the new scans.
- [x] What the backends answer to `GET /v1/crashes`, from their code in a
      throwaway copy with the backend's test client (no network):
      `xerces-design` (what Railway runs) 404, `launcher/crash-backend` 405.
- [x] `xerces_cli crash send` against a stand-in on 127.0.0.1 (`probe_stub.py`):
      answering as `xerces-design` does, the pending crash was queued, the
      client asked `GET /v1/crashes` with no install id, got 404, sent nothing,
      printed `waits` and exited 5, and no `install_id` was made; answering as
      `crash-backend` does (405, then 201), the report went with a new install
      id and the outbox emptied.
- [x] First run on a fresh home: "Skip to Privacy" opens step 04 (logged
      `launcher.first_run.step 4`), Finish opens Home and saves
      `first_run_done`; English and pseudo captured. The privacy step, the
      privacy section of Settings, Home and Friends show the new texts;
      `check-screens.ps1` (dark, light, pseudo): every screen captured, no RmlUi
      warning or error.
- [x] `THIRD_PARTY_NOTICES.txt` (`notices.py`): UCDN's and fasthash's headers
      and the one copyright line `COPYING` does not list (Elie Roux) in
      HarfBuzz's entry, `Containers/LICENSE.txt` in RmlUi's, the BDF and PCF
      drivers' and `fthash.c`'s licences and `pcfutil.c`'s Open Group notice in
      FreeType's, the ProggyClean and ProggyForever fonts and the stb parts in
      Dear ImGui's; the scan finds no other compiled third-party file under a
      licence it does not know.
- [ ] The UPnP retry for a stale "Xerces lobby" mapping and the launcher's
      "waits on this PC" notice: built, not run (a router and a real send are
      needed; dry launchers never send).
- [x] `tools\release.ps1` on the clean committed tree (it warned that HEAD is
      not on origin) passed every gate, the fence, licence and Microsoft notice
      lints among them, and wrote `dist\xerces-0.1.0-beta.1.zip` (232 files, 9.4
      MB: the old launcher's 1.6 MB is gone), `SHA256SUMS-0.1.0-beta.1.txt` and
      the symbols zip. `README.txt` and `KNOWN_ISSUES.txt` equal
      `docs/TESTERS.md` and `docs/KNOWN_ISSUES.md` (CRLF, no byte-order mark);
      `data\launcher` holds only the one-page `main.html`;
      `THIRD_PARTY_NOTICES.txt` has the new texts above.
- [x] The zip unzipped outside the repository, `SHA256SUMS.txt` matching all 232
      files, its `Xerces.exe --dry-launch` started from another folder on a
      fresh home: `launcher.start` names the package's `data` as `client_data`,
      HarfBuzz runs, Skip to Privacy and Finish work, the first scan read 17
      items, and Launch vanilla logged `launcher.launch.dry`
      (`-launch -nointro`) with `xerces_data_path` set to the package's `data`
      folder; About shows the BETA badge and the commit; no warning or error in
      the log.

### Beta game session (2026-11-06)

With your go-ahead ("Yes, run it now"), run by Claude with the PC otherwise
idle: `C:\Users\Ginld\XercesDev\bench\20261106-beta\session.ps1` and
`session2.ps1` (logs next to them), `game-bench.ps1` (new: `-BinDir` for an
unzipped package, `-HoldMouseDuringLoad`), `m1-check.ps1 -Live`. The game's
`config.ini` and `config.cfg` were backed up first and were byte for byte the
same afterwards (the borderless runs put `config.ini` back after each run).

Run 1 (d399e946) found a crash in every run, 2 s after the menu or a map load:
chat.cpp's `note_client_names` (crash-report redaction) passed a null name to a
string. `utils::string::va` returns null for an empty result, so an empty player
slot crashed it. Because the crash handler had run, every quit then failed
m1-check's clean_exit check. Fixed in 5ae63145 (empty names are skipped); the
package was rebuilt from it (`dist\xerces-0.1.0-beta.1.zip`, 9.4 MB, SHA-256
`2f6b5edcf43042d4336386fde3b7fb7f0452cbc68ce0d7bc06cc993ab36c94ac`). Run 1's
crash report reached the backend (server id
`fefaa15c-67f8-4458-9abe-b36205734574`).

Run 2 (5ae63145):

- [x] The unzipped package (`C:\Users\Ginld\XercesBeta`, started through its own
      `xerces_cli.exe` with a fresh home): vanilla Cheese Cube loaded, played
      and returned to the menu (292 FPS mean over 30 s); with the D3 stack too
      (315 FPS). No crash, no dialog.
- [x] The menu crash's trigger (mouse-move messages with both buttons down,
      posted to the game window every 100 ms from its creation until 3 s after
      the menu, 145-151 posts): no crash in the package or the repo build; the
      package's run logged `xerces.input.active_device_dropped` once (the repo
      run's posts missed the window where the primary client is -1).
- [x] A forced crash (`-xerces-crash-test`, `xerces_crash_test`) with "Send
      crash reports" on: the dialog is titled "Xerces"; the pending note and the
      620 KB stack dump were written; `xerces_cli crash send` sent it and the
      live backend answered 201 (server id
      `8394abfa-e6ce-48a2-be17-17f42748545e`).
- [x] `m1-check.ps1 -Live`: vanilla, crash, restart, safeoff, reopen, dvars, cli
      and cliapply pass; modded failed once on "quitting leaves clean_exit": a
      0-byte dump was written as that game quit (a crash while shutting down;
      its log had rotated out). Six more modded quits with crash reports on
      (`quit-repro.ps1`) all ended at clean_exit. So 1 of 12 modded quits; cause
      unknown (KNOWN_ISSUES).
- [x] Flip model, borderless, uncapped (`b0` vs `b0flip`, 3 runs each, A-B-B-A):
      `xerces.flip_model` applied FLIP_DISCARD with tearing in every flip run,
      no black screen or crash. No measurable gain: 312.5 vs 310.6 FPS (-0.6%, p
      0.84), p99 4.08 vs 4.33 ms (p 0.25), 1% low 210.5 vs 209.4. Whether the
      borderless window got independent flip needs PresentMon (an elevated
      terminal).
- [x] Precise limiter, borderless, `com_maxfps 240` (`b240` vs `b240p`): worse.
      239.9 vs 234.6 FPS (-2.2%, p 0.008), p99 6.12 vs 6.52 ms (+6.6%, p 0.007),
      1% low 145.3 vs 142.8, 0.1% low 110.2 vs 114.2 (p 0.21). Its own stats:
      55% of frames released within 100 us of the deadline, p99 340 us late.
      Each next deadline counts from the late release, so the lateness adds up
      every frame (235 instead of 240). Next step: count from the previous
      deadline and resync only after a frame a whole period late, then measure
      again. It stays off by default.
- [ ] Two PCs (`docs/TWO_PC_TEST.md`), PresentMon, a person playing the beta.

### OneDrive crash (2026-11-06)

You played the beta zip (9bcbe8ab) unzipped into
`C:\Users\Ginld\OneDrive\Documents\My Games\xerces-0.1.0-beta.1`. Every launch
crashed about 8 s in, while launching: modded, the culprit search's tests and
vanilla alike.

- **The crash.** All 34 dumps in `%LOCALAPPDATA%\boiii\minidumps` (15:03 to
  15:11) hold `weakly_canonical: Access is denied.` for `boiii.start` next to
  `boiii.exe`. The game opens that file through `_fsopen`; Xerces' path hook
  (`patches.cpp` `fsopen_adjustpath`, `path::normalize`) resolved it with the
  throwing `std::filesystem::weakly_canonical`, and the exception ended the
  game. Why Windows refused the file in OneDrive is not known (the file was gone
  afterwards). Copied to `C:\Games\xerces-0.1.0-beta.1`, the same zip started
  and loaded a map.
- **The flood.** The crash handler resumes the game after a crash (Rip to
  `reset_state`), which Windows cannot do for a C++ exception: it raised
  STATUS_NONCONTINUABLE_EXCEPTION (0xC0000025), which came back to the handler.
  Each of the 6 launches wrote 3 to 13 dumps, zips and crash reports, about 3 s
  apart, until the game ended (exit code 1). The first report of each launch has
  0xE06D7363, the rest 0xC0000025. More than 20 reached the backend; its
  per-install limit answered 429 to the rest, which waited in the outbox.
- **A second copy.** Started from `C:\Games` while the copy in OneDrive was
  open, Xerces.exe only brought the open window forward (one launcher per home),
  so two more launches still ran from OneDrive.

Fixed on `launcher/onedrive-fix`:

- `path::normalize` and the folders it compares with use the `error_code`
  overloads: a path Windows will not resolve goes to the game's call as it came,
  and the game log gets one `path.unresolved` line per game.
- A noncontinuable exception gets the crash dialog in the handler instead of the
  resume; at most three crash reports per game (a server plays on after a
  crash).
- Home warns for 8 s when Xerces.exe is in a OneDrive folder
  (`launcher.install.onedrive`, `onedrive_folder_of`, unit-tested).
- A second copy of Xerces.exe started from another folder says which folder the
  open one is in.
- [ ] The fixed client from a OneDrive folder in the game: does the game start
      when Windows refuses `boiii.start`?

In the same play from `C:\Games`, `zm_mw4_shipment` with DS4C, Classic Viewmodel
FOV and Hyperdimension Neptunia ran out of zone memory while loading
(`loader.out_of_memory`, exit 0xFFFFFFFF): the known DS4C limit
(KNOWN_ISSUES.md). Larger zone memory was off.

### Mod labels (2026-11-06)

With your go-ahead ("Now, I'm not playing"), run by Claude from the worktree
build (`launcher/mod-labels`, through `xerces_cli plan apply` with a test home
in `C:\Users\Ginld\XercesDev\bench\20261106-modlabels`, your stack: DS4C,
Classic Viewmodel FOV, Hyperdimension Neptunia; Larger zone memory on; keys and
captures through `tools\dev\session`). A temporary probe script (not committed)
logged each menu, list data source, item widget and the items' model fields.

What the menus are:

- Zombies weapon kits (DS4C's Zombies menu, the item below Dr. Monty's Factory)
  -> a weapon -> the camo slot opens the game's `GunsmithCamoSelect`: list
  `GunsmithWeaponOptions`, item widget `CustomizationItem_ND`, tabs
  `CACCustomizationTabs` (DS4C renames the five tabs to its own categories).
  Each item's `image` is the camo icon, such as `menu_camo_zm_dlc2_pap01`.
- DS4C's pause menu -> Camos: `T7StartMenu_UISettings_Main`, lists
  `DS4C_Camos_*`, item widget `T7StartMenu_UISettings_CamoOptions_ListItem`,
  `image` and `camoIndex`; DS4C's BO6/BO7/MW3 and Reactive tags sit at the top
  of each icon.
- DS4C's pause menu -> the next entry: the character screen, list
  `UISettingsCharacterOptions`, item widget
  `T7StartMenu_UISettings_CharacterOptions_ListItem`, `image` (`icon_dempsey`)
  and `characterIndex`.

Owners: `game.getassetowner("image", ...)` gave DS4C (3171632315) for every camo
and character icon in those lists and nothing for the game's own icons
(`t7_icon_weapon_smg_fastfire`). The first build also gave an owner for names
that are no image ("Topaz", the camo's name): the game hands out a stand-in for
those, and the stand-in's zone was Neptunia's in the menu and Cheese Cube's in
the match. Fixed with `DB_IsXAssetDefault` (db.cpp already uses it).

- [x] The weapon kits' camo picker: "DS4C Patched" across the top of each DS4C
      camo; no errors in the game log.
- [x] DS4C's Camos in a match (Cheese Cube): "DS4C Patched" along the bottom,
      clear of DS4C's own tags.
- [x] DS4C's Characters: "DS4C Patched" across the top of each portrait.
- [x] Another camo or character mod in the stack: with DS4C, Ascendance,
      Hyperdimension Neptunia, Avali and im_oxo QOL (after the Ascendance fix,
      below), DS4C's camo list and character list win and every label reads
      "DS4C Patched".
- [ ] The Multiplayer pickers.

The Season Pass bonus overlay (Dr. Monty's Factory) was closed by quitting the
game, not with OK. Windows asked whether the worktree's boiii.exe may use the
network; that prompt was left for you.

### Ascendance at start (2026-11-06)

You asked for a test of 0.1.0-beta.3 with DS4C Patched, Ascendance (Aesthetic
oJumpy, 3000417367), Hyperdimension Neptunia, Avali: Solspace and im_oxo Zombies
QOL. Run by Claude with the beta.3 package and a test home
(`C:\Users\Ginld\XercesDev\bench\20261106-modlabels`, `bisect.ps1` and
`probe-run.ps1` there).

- **The crash.** Every launch with Ascendance crashed about 5 s after the window
  came up, before the menu: Ascendance alone, with DS4C in either order, Larger
  zone memory on or off (EXCEPTION_BREAKPOINT at BlackOps3.exe+0x141D3C84B,
  after "LobbyVM script execution error ... lua/shared/luareadonlytables.lua:1:
  function expected instead of nil"). DS4C with im_oxo, with Avali, and with
  both character packs reached the menu.
- **Why.** Ascendance ships its own `lua/shared/luareadonlytables.lua` (17,419
  bytes, its menu colours in front of the game's read-only-table code), which
  calls `EnableGlobals()` on its first line. A temporary probe ran a check in
  each Lua engine as it loaded: the menus' engine has `EnableGlobals`,
  `DisableGlobals`, `CoD` and `LUI`; the lobby's engine has none of them and no
  metatable on `_G`, in every start. Xerces loads the mod stack as the game
  starts (`fs_game`), before the lobby engine first runs, so the lobby engine
  gets the mod's copy (the game's own copy, 659 bytes in `core_patch`, stays
  linked behind it, not in use). A mod loaded from the Mods menu after the lobby
  engine started would not reach it.
- **The fix** (`ui_scripting.cpp` `try_mod_copy_in_lobby`, `mod_stack.cpp`
  `game_copy`): in the lobby engine, a module whose copy in use is a mod's is
  first required in protected mode. If it fails, the lobby engine reads that
  file from the game's own zones from then on (in the Lua file reader, for that
  read only; its loader gets the file without `Lua_CoD_GetRawFile` or
  `DB_FindXAssetHeader`), the menus keep the mod's copy, and the game log says
  `lua.lobby.game_copy` with the file, the mod and the error. The lobby's
  `require` keeps a module under its lower-case name and marks it loaded before
  it runs, so both names are cleared before the retry. A mod's copy that works
  in the lobby engine is kept, as upstream added for AAE's lobby scripts.
- [x] Ascendance alone: the menu, in Ascendance's blue theme with its own
      entries ("DISCORD SERVER").
- [x] All five: the menu (DS4C's, with Ascendance's highlight colour), the
      weapon kits' camo picker with "DS4C Patched" labels, a match on Cheese
      Cube with DS4C's Camos and Characters, no Lua error and no crash.
- [ ] Ascendance's `T7AAO.dll` (not on the block list): nothing in the console
      showed it loading in these runs.

With DS4C and Ascendance together, the Vesper's preview showed an orange camo
for DS4C's "Topaz" (yellow without Ascendance): DS4C's camo list wins, but
Ascendance also ships per-gun camo assets, and where only Ascendance has a gun's
camo asset its looks follow Ascendance's numbering (the research of 2026-11-06
predicted this; KNOWN_ISSUES.md).

### PEGASUS and the mod menu entry (2026-11-07)

You reported a crash with PEGASUS (2804432292) in your stack (crash report
6dce0b83 on the backend: an access violation reading address 0 at
BlackOps3.exe+0x15CC412CF, 5 s after the front end). Reproduced with your
go-ahead; the crash needs Enter on the start screen.

- **Why.** PEGASUS's own `ui/uieditor/menus/lobby/lobby.lua` compares
  `Engine.DvarString(nil, "fs_game")` with 2804432292, 3674642576, 3765563609
  and "pegasus" and runs `Engine.Exec(c, "quit")` otherwise. Below DS4C the game
  quits while it builds the menu and crashes doing so. PEGASUS alone and PEGASUS
  on top of DS4C reach the menu. The check is the mod author's, so Xerces leaves
  it alone (no faked `fs_game`).
- **PEGASUS on top of your stack** (0.1.0-beta.4, PEGASUS, DS4C, Classic
  Viewmodel FOV, Hyperdimension Neptunia, im_oxo QOL, Avali, Ascendance; test
  home in `C:\Users\Ginld\XercesDev\bench\20261106-modlabels`): menus, weapon
  kits and a Cheese Cube match, no crash. The front end is PEGASUS's (DS4C's
  PROGRESSION entry is gone), the main menu's Multiplayer button reads "Legacy
  camos" (DS4C's text), the Zombies camos in the weapon kits are tagged PEGASUS,
  and the match has PEGASUS's HUD with DS4C's pause menu.
- **PEGASUS's menu.** Its button is in its own copy of
  `ui/uieditor/widgets/startmenu/startmenu_gameoptions_zm.lua` (list
  `ModStartMenuGameOptions`, "^9MOD OPTIONS" ->
  `NavigateToMenu(..., "StartMenu_ModSettings_Main", ...)`). DS4C's pause menu
  (`t7startmenu_main.lua`, tabs `StartMenuTabs_Death`) uses its own
  `StartMenu_GameOptions_ZM_Death` with list `StartMenuGameOptions_Death`, so
  the button never shows. Avali ships the same template ("^6Character Menu"),
  hidden the same way under DS4C. Opened with a test `lua_exec` file
  (`NavigateToMenu` from the open `StartMenu_Main`, a child of
  `LUI.roots.UIRoot0`; BO3 has no `LUI.FlowManager`), PEGASUS's menu worked:
  Camo and Character tabs, and Back returned to the pause menu.

Built on `launcher/mod-menu`: `data/ui_scripts/mod_menu` (DESIGN.md 7.6) and the
planner's `plan.stack.top_only` warning with a "Move to the top" fix (DESIGN.md
5.3 step 4). Run by Claude with your go-ahead ("Yes, I'm not playing"), from the
worktree build through the same test home:

- [x] PEGASUS on top of your stack: "PEGASUS MENU" between Mod Options and
      Restart Level in DS4C's pause menu, all six rows on screen; it opens
      PEGASUS's Camo and Character menu, Back returns to the pause menu, and the
      entry shows once when the pause menu opens again.
- [x] Your stack without PEGASUS (DS4C on top): "AVALI MENU" in DS4C's pause
      menu opens Avali's Character Select; Back returns.
- [x] PEGASUS alone: its own pause menu with its own "MOD OPTIONS" and no added
      entry.
- [x] No Lua error in the console in any of the three; no crash dump.
- [x] Launcher, test home with DS4C above PEGASUS: Home counts 1 warning; the
      Mods screen shows the warning under PEGASUS in the load order and in its
      details, with "Move to the top". Clicking it put PEGASUS first (saved in
      the test home's `launcher.json`), the plan showed 0 warnings, and the
      status line read "[PZM] PEGASUS v0.2.3.9 moved to the top".
- [x] `xerces_cli plan resolve` with copies of the two mods' `workshop.json`:
      the warning and `fix: move_to_top ws.2804432292` with DS4C on top, nothing
      with PEGASUS on top.

In the matches with PEGASUS, the game asked whether to allow unsafe Lua
functions (PEGASUS opens a file); Claude answered No each time, which keeps them
blocked, and the match played. With DS4C on top and no PEGASUS the match starts
without a "press a key" prompt: a test script that waited for one left the
player standing until the zombies ended the game, so it now pauses as soon as
the player is in.

### Join from Servers after the main menu (2026-11-07)

A tester's game crashed three times in a row joining from Servers (crash reports
on the backend: a null read in `Storage_SerializeTransferData` under
`CL_CheckForResend`, about 90 s after launch), while `connect` typed at the main
menu joined the same host. Built on `launcher/join-after-menu` (DESIGN.md
11.10). Run by Claude with your go-ahead ("Yes, run it"), from the worktree
build through the test home in
`C:\Users\Ginld\XercesDev\bench\20261106-modlabels`, with
`+set xerces_join 203.0.113.7:27017` (a documentation address nothing answers)
passed through the home's `dvars` setting: the same argument Join now passes
(`C:\Users\Ginld\XercesDev\bench\20261107-join\join-run.ps1`).

- [x] The first build crashed 8 s in, in the component's `post_unpack`: it
      registered the dvar before the engine's dvars exist. Fixed: the dvar is
      registered on the main thread and the address read from the command line.
- [x] Without mods: the command line had `+set xerces_join` and no `+connect`.
      The game waited on the title screen (no main menu until a key) and joined
      (`xerces.join.connect` in the game log) 3.8 s after Enter; the console
      said "Connect failed: No response from server", and the game stayed at the
      main menu. No crash dump in over 3 minutes.
- [x] Your stack (PEGASUS on top, 7 mods): PEGASUS's unsafe-Lua prompt opened as
      the main menu loaded and held it until answered (No); the join came 3.6 s
      later, the game showed its "Connect failed" toast and stayed at the menu.
      No crash dump in over 3 minutes.
- [ ] A join to a real host from Servers, on two PCs. Only a host that answers
      reaches the handshake where the tester's game crashed, so this is the
      check that shows the crash is gone.

### MW4 Shipment out of memory (2026-11-07)

You reported "ran out of memory" on MW4 Shipment (Workshop 3829169329) with your
six mods even with Larger zone memory on. Your game logs had four such exits,
all `loader.out_of_memory` with zone `zm_mw4_shipment` and caller 0x20C5A3D (the
pool allocator's bounds check): DS4C alone, two and three mods with the stock
pool (2026-11-06), and the six mods with the larger pool right after Shadows of
Evil (2026-11-07). The zone headers: MW4 Shipment's main zone takes 736.3 MiB of
blocks 5 and 6, so DS4C alone should have fit the stock main pool with 11 MiB to
spare (849 + 451.3 + 736.3), and the six mods (1169.3 MiB) the 4 GiB one with
1.3 GiB to spare.

Run by Claude with your go-ahead ("Yes, I'm not playing"), on the
`launcher/oom-diag` worktree build through the test home
(`C:\Users\Ginld\XercesDev\bench\20261106-modlabels`, `mw4-run.ps1` there),
Larger zone memory on unless said:

- **Where it ran out.** The six mods straight into MW4 Shipment: the main pool
  held 1334 MiB in the front end, 1907 as the level started and 2753 of 4096 at
  the exit, so it was not full. With every pool listed and the allocator's own
  shortfall and descriptor read at the exit, the full one was pool 2, the DB
  stack's `gpu_prt` (18 GiB), short by 5950 MiB. DS4C alone with that pool at
  its stock 18 GiB: short by 1322 MiB, the main pool at 2035 MiB (the main-pool
  figures were right all along). Block 8 of the zones (streamed images: 10.4 GiB
  for MW4 Shipment, 2.7 GiB for DS4C) fills that pool; both runs give a stock
  baseline of 6338 MiB (the six-mod run predicts 5951.5 MiB short).
- [x] The six mods with the first `gpu_prt` raised to 30 GiB: MW4 Shipment loads
      (46 s), the level renders with its textures (screenshots), the map's own
      mode menu opens, and the pool holds 778 MiB committed in the level.
- [x] Your sequence: Shadows of Evil first (2410 MiB in the main pool), back to
      the menu, then MW4 Shipment: it loads. The main pool goes back to 1906 MiB
      in the front end between the maps.
- [x] The finished build: `xerces.db_pool` says "its streamed-texture pool 30720
      MiB (stock 18432 MiB)", and the six mods load MW4 Shipment again.
- [x] The plan (`xerces_cli plan apply --dry-launch` on the test home, DS4C
      alone): with the stock pools, "MW4 SHIPMENT ZOMBIE ... needs 19754.1 MiB
      of streamed-texture memory, 1322.1 over" and the warning offers Larger
      zone memory; with it, every installed map fits.

In the matches, MW4 Shipment's own menu script asked for unsafe Lua; Claude
answered No each time. Windows Firewall asked whether the worktree's boiii.exe
may use the network; that prompt was left for you.

### Zombies round settings, stage 1 (2026-11-07)

DESIGN.md 7.9, built on `launcher/round-system`. Run by Claude with your
go-ahead ("Yes, I'm not playing") on Kino der Toten (`zm_theater`, it has a
power switch, doors, debris, wall buys and a moving box), from the worktree
build through the test home `C:\Users\Ginld\XercesDev\bench\20261106-modlabels`.
The runner is `C:\Users\Ginld\XercesDev\bench\20261107-rounds\rounds-run.ps1`.
It passes all eight settings on (5,000 starting points) as launcher dvars, so
they were `+set` for that session only and the game's `config.cfg` kept its own
lines (`xerces_launch_dvars`). A test-only game script and Lua file, copied into
the worktree's `data\` for each run and removed after it, reported what the
settings did as game messages. The test script gave a gun from the map's list
through `zm_weapons::weapon_give` (as a wall buy does), emptied the gun in hand
and ended round 1 by killing the zombies.

- [x] No mods: the score is 5,000 and the perk limit 99 when the black screen
      ends; the power is on (the switch was used and removed); all 12 door
      triggers and every debris pile are open; `zombie_unlock_all` is back to 0;
      the box's move chance is overridden; the gun came Pack-a-Punched (the base
      gun gone, two primaries); the emptied gun had 208 rounds in stock 1 s into
      round 2.
- [x] No mods: "Xerces settings: Max Ammo every round, Auto Pack-a-Punch, 5,000
      starting points, No perk limit, Box stays, Power on, Doors open, Round
      HUD" in the game messages.
- [x] No mods: `xerces_zm_match` was "127 5000" and `xerces_zm_hud` went from "8
      0 1" to "8 5 1" in round 2, the same in the host's Lua, where the round
      HUD element exists.
- [x] No mods: the pause menu reads Resume, Direct Connect, Change Map, Kick
      Players, Restart, Game Tweaks, XERCES SETTINGS, Main Menu, Quit to
      Desktop, and its read-only XERCES SETTINGS opens.
- [x] Front end, with and without mods: XERCES SETTINGS follows BOIII SETTINGS
      in both Zombies lobbies (with your stack: after DS4C's Barracks, Join
      Discord and BOIII SETTINGS), and the menu opens.
- [x] Your stack with PEGASUS on top (PEGASUS, DS4C, Classic Viewmodel FOV,
      Hyperdimension Neptunia, im_oxo QOL, Avali, Ascendance): every result
      above is the same, and DS4C's pause menu reads Resume, DS4C Camos, DS4C
      Mod Options, XERCES SETTINGS, PEGASUS MENU, Restart Level, End Game.
- [x] No script error from Xerces' script in any run. With the stack, DS4C
      prints its own errors while the map loads (also `_fixed_box.gsc`, before
      any setting acts) and when the test script kills zombies without an
      attacker.
- [x] How it looks, seen once your PC was unlocked (the first runs were made
      while it was locked, and every screenshot came out black), from
      full-screen captures (`capture-dpi.ps1`: the screen is 3840x2160 at 150%,
      and the session's `capture.ps1 -Screen` only saw its top-left 2560x1440).
      The lobby screen shows all eight settings and the description panel. In
      the match the round HUD sits top left (grey ZOMBIES LEFT and ROUND TIME or
      LAST ROUND, white values), no timer line before round 1. The game's own
      "Max Ammo!" shows at each round's start with the stock refilled. The
      settings message is one line in the game messages. The pause menu's
      read-only XERCES SETTINGS lists all eight with each description in full.
      With your stack the HUD stays clear of PEGASUS's and DS4C's panels (level,
      tokens, the Blue Door tracker).
- [x] Found and fixed there: the added pause entries pushed the last row out of
      view: Quit to Desktop in the stock pause menu, End Game in DS4C's. Both
      entries (XERCES SETTINGS and the mod menu entry) now grow the list. The
      stock list grows through BOIII's own row count; DS4C's list, whose rows
      are as high as the stock ones (33.6), grows a row per added item. Seen
      after the fix: all nine rows without mods, all seven in DS4C's pause menu.

Tried and taken out: registering the settings event (`xerces_settings`) and the
HUD labels with the engine as the map loads (DESIGN.md 7.9). The server's and
client's lists then differed ("BG Cache Diff for LocalizedStrings - Mismatched")
and the game left the map at once.

### Zombies round settings, stage 2 (2026-11-07)

DESIGN.md 7.9 stage 2 (the Pack-a-Punch wheel and the coin flip), on
`launcher/round-system`. Run by Claude with your go-ahead ("Yes, I'm not
playing") on Kino der Toten, the same way as stage 1, through
`C:\Users\Ginld\XercesDev\bench\20261107-rounds\stage2-run.ps1`: all ten
settings on as launcher dvars, and a test-only game script (`probe3\`, copied
into the worktree's `data\` for each run and removed after it) that gave a gun
from the map's list (Auto Pack-a-Punch upgraded it), kept the test player from
going down (it stands among the zombies), ended rounds by killing the zombies,
and reported as game messages. The first runs used the pause menu entries and
the bet menu through Lua, calling each row's action the way selecting it does;
the later ones pressed the arrow keys and Enter. All took full-screen captures.

- [x] Lobby, with and without mods: XERCES SETTINGS has a ZOMBIES tab (stage 1's
      eight) and a GAMBLING tab (Pack-a-Punch Wheel and Coin Flip, each with its
      description). The runner changed tabs the way the controller's shoulder
      buttons do (the tab grid's `navigateItemRight`; the game's tab list has no
      keyboard key, and a mouse click goes to the same grid), and the list and
      its heading changed to GAMBLING.
- [x] No mods, with real keys: the pause menu reads Resume, Direct Connect,
      Change Map, Kick Players, Restart, Game Tweaks, XERCES SETTINGS,
      PACK-A-PUNCH WHEEL, COIN FLIP, Main Menu, Quit to Desktop, all in view.
      The arrow keys and Enter on PACK-A-PUNCH WHEEL closed the pause menu and
      spun the wheel. On COIN FLIP they opened the bet menu (BET 100 to BET
      50,000 and ALL IN, "Heads wins 200 (x2.0)" at round 1), where Enter on BET
      1,000 flipped. On XERCES SETTINGS they opened the read-only list of all
      ten settings, where Enter on a row did nothing. The runner pressed Enter
      only once a Lua readout said that row had focus.
- [x] The wheel, no mods: 2,500 points a spin. A second spin in the same round
      was refused ("One spin a round: spin again next round") with nothing
      taken. During a Fire Sale (the game's own flag, set by the test script) a
      spin cost 10 and the wheel's middle said FIRE SALE: 10. On nothing the
      box's teddy bear rose and turned in front of the player. It refused the
      starting M1911 and a score of 1,000, taking nothing. Across the runs it
      gave each of the five ammo mods and nothing (Turned only came up after 25
      spins, with your stack). One run had five nothings in a row (a 0.7%
      chance), so the stack run counted the game's `RandomInt(8)` 4,000 times:
      532, 501, 487, 483, 465, 505, 530, 497, as even as chance allows
      (chi-square 7.3, 7 degrees of freedom).
- [x] The coin flip, no mods: BET 1,000 at round 1 lost once ("COIN FLIP:
      1,000", TAILS, "YOU LOSE 1,000" in red) and won twice ("YOU WIN 2,000
      (x2.0)" in green). In round 2 (x2.1), after a win: 100 from 5,000 ends at
      5,110 ("YOU WIN 210 (x2.1)", with the game's own +210 by the score), 1,234
      from 5,000 at 6,357, 987,654 from 3,000,000 at 4,086,419, and 100,000 from
      4,100,000 at 4,194,303 ("YOU WIN 194,303 (MAX POINTS)"). Refused, with
      nothing taken: 50, 6,000 with 5,000 points, "abc", and any bet at
      4,194,303 points.
- [x] The game's points cap, found by these tests: a score a script sets to
      1,500,000,000 reads back as 4,194,303. The coin flip now stops there
      (DESIGN.md 7.9).
- [x] No mods: after the pause menu's restart (`fast_restart`, which RESTART
      runs in a solo game) the round HUD shows once, not doubled, and the pause
      menu has the three entries again.
- [x] Your stack (PEGASUS on top, DS4C's pause menu), with real keys: the pause
      menu reads Resume, DS4C Camos, DS4C Mod Options, XERCES SETTINGS,
      PACK-A-PUNCH WHEEL, COIN FLIP, PEGASUS MENU, Restart Level, End Game, all
      in view. Two soak runs of 8 rounds each pressed only keys, with no Lua
      sent from the console. Every round Down and Enter spun the wheel, then
      picked COIN FLIP and a bet; the second soak also opened XERCES SETTINGS
      every round (Enter on a row did nothing) and PEGASUS MENU every other
      round (its Camo and Character menu). No crash, no Lua error and no script
      error from Xerces in either. The payouts on screen were right (5,000 at
      x2.1 paid 10,500; ALL IN with 10,290 at x2.8 paid 28,812; 7,500 at x2.4
      and at x2.6 paid 18,000 and 19,500), the wheel took nothing for a Death
      Machine from a power-up, and the bet menu reopens on the row last picked.
- [x] Your stack, earlier runs driven by test Lua: the lobby's GAMBLING tab; the
      wheel landed on Turned, Dead Wire, Thunder Wall and Blast Furnace; a bet
      of 1,000 won ("YOU WIN 2,000 (x2.0)"); the Fire Sale spin cost 10; and the
      HUD stays clear of PEGASUS's and DS4C's panels.
- [x] How it looks, from full-screen captures. The wheel is a dark square in the
      middle of the screen under an orange PACK-A-PUNCH WHEEL, with the ammo
      mods' HUD icons and three NOTHING round the price; an orange highlight
      goes round three times, slows onto the result, and the result's name shows
      in the middle in its colour. The coin flip is a smaller panel: COIN FLIP
      and the bet in orange, the coin's side turning in gold, then YOU WIN in
      green or YOU LOSE in red. Both close by themselves. The round HUD is top
      left for every player, as in stage 1. With PEGASUS the lobby screen has no
      title (its restyle of the settings screen), in stage 1 too.
- [x] No script error from Xerces' scripts in any run, and no Lua error from
      Xerces' menus. With your stack, DS4C's progression script
      (`scripts/ds4c/progression/live_xp.gsc`) prints "LUI event name (string)
      is not precached" as zombies die, as it did in stage 1.

Found and fixed during these runs:

- A menu response ends at its first space, so in the first run the bet went to
  the game as `xerces_flip` alone and no flip ran. Responses now join their
  parts with ":".
- GSC multiplies in single-precision floats, so a payout came out a point short
  for about 1 bet in 100 (100 at x2.1 paid 209). The payout is now worked out in
  whole numbers, checked offline against exact arithmetic for about a million
  bets, and in the game above.
- A win past the points cap was cut by the game without a word; it now says MAX
  POINTS, ALL IN's description names the cap, and a bet at the cap is refused.
- From a review of the code: under the Shopping Free gum a spin whose gun was
  gone gave back 2,500 points it never took (it now gives back what it took);
  the once-a-round lock would have stuck at round 255 (it counts rounds started
  now); between rounds the flip paid 0.1 more than the menu showed (both use the
  round as of its start now); a refused request said nothing once the pause menu
  had closed; the teddy bear stayed in the world if its player left mid-rise;
  and an ammo mod without a HUD icon would have broken the wheel (it shows the
  name now).

Crashes in the test runs: four of the seven runs with your stack that sent Lua
through the console (to read the menus and pick rows) crashed in the game's Lua
VM, twice at BlackOps3.exe+0x141D3DAA3 writing near address 0, once in its
garbage collector on another thread, and once with no crash dump. None did
without mods (four runs), and none did in the two soaks above, which only press
keys, so they are put down to the test's own Lua. The address is noted in case
it turns up in a player's crash report.

Not tested yet: the refund under Shopping Free, rounds past 255, an ammo mod
without an icon, and everything that needs a second player (KNOWN_ISSUES.md).

### Zombies round settings, stage 3 (2026-11-07)

DESIGN.md 7.9 stage 3 (round skips, random loadouts, self-revives, rounds to
999), on `launcher/round-system`. Run by Claude with your go-ahead ("you can
start now") on Kino der Toten, through
`C:\Users\Ginld\XercesDev\bench\20261107-rounds\stage3-run.ps1` (no mods) and
`soak3-run.ps1` (your stack). Settings on as launcher dvars: Round HUD, Round
Skips, Random Loadout, Self-Revives and Rounds to 999. A test-only game script
(`probe5\` without mods, `probe6\` with your stack, copied into the worktree's
`data\` for each run and removed after it) kept the test player standing,
reported as game messages, forced a dog round (`level.next_dog_round`), downed
the player, and moved the game's round to its 255 cap and Xerces' count to 998
for the last vote. In the match only keys were used: Down and Enter on the pause
menu by row count, and the use key held 2.3 s for the self-revive. Lua ran from
the console only in the lobby. Captures are of the game window alone
(`capture-game-dpi.ps1`), so they cannot show anything else on your screen.

- [x] Lobby, without mods: XERCES SETTINGS has ZOMBIES (now with Random Loadout
      and Self-Revives), GAMBLING and a ROUNDS tab with Round Skips and Rounds
      to 999, each with its description.
- [x] Round skips, without mods. Round 2 (prime): "PRIME ROUND 2: SKIP IT FROM
      THE PAUSE MENU FOR 400 POINTS" under the HUD, SKIP ROUND after XERCES
      SETTINGS in the pause menu; Enter on it ended the round with exactly 400
      points and no Max Ammo dropped. Round 3 (prime) through the menu response:
      the same. Round 4, a dog round: "SPECIAL ROUND: ... AND A MAX AMMO"; Enter
      on SKIP ROUND once its dogs were out ended the round with 400 points and
      the round's own Max Ammo dropped ("ROUND SKIPPED: 400 POINTS, AND THE
      ROUND'S MAX AMMO"). Round 6 (neither): the request was refused with
      nothing given.
- [x] Random loadout, without mods: from round 1 every round swapped both guns
      for two box guns (among them a Ray Gun, the Thundergun, rifles, SMGs,
      shotguns, a launcher), different each round.
- [x] Self-revives, without mods: the HUD's SELF-REVIVES 1; downed (solo, no
      Quick Revive) the game went on, HOLD USE TO REVIVE YOURSELF showed, and
      holding use got the player up with 0 left; round 10 put it back to 1.
- [x] Rounds to 999, without mods: with the game's round at 255 and Xerces'
      count at 999 the HUD shows ROUND 999 (the game's own chalk stays at 255),
      "ROUND 999: EXFIL OR KEEP GOING? VOTE IN THE PAUSE MENU", and EXFIL and
      KEEP GOING in the pause menu. KEEP GOING: 8 of 8 perks, a full clip, the
      score at 4,194,303 and back there after spending. A game over then shows
      GAME OVER and YOU SURVIVED 999 ROUNDS above the game's scoreboard (the
      game's own would say 255). EXFIL (a second run): the game ended with
      EXFIL, YOU WIN and YOU SURVIVED 999 ROUNDS.
- [x] Your stack (PEGASUS on top), keys only, 8 rounds: DS4C's pause menu reads
      Resume, DS4C Camos, DS4C Mod Options, XERCES SETTINGS, then SKIP ROUND on
      a round that can be skipped (EXFIL and KEEP GOING at round 999), PEGASUS
      MENU, Restart Level, End Game, all in view. Down and Enter on SKIP ROUND
      skipped rounds 2, 3 and 5 (prime) and a dog round at 7 (special), 400
      points each. The self-revive got the player up (a round that started while
      they were down swapped their guns once they were up). Every round gave two
      box guns, with DS4C's attachments on them. At round 999 KEEP GOING held
      the score at 4,194,303. No crash, no Lua error. DS4C's copy of
      `_zm_weapons` printed a script error ("pair 'undefined' and 'bo6'") when
      the random loadout handed over a gun; the gun came as usual.
- [x] No script error from Xerces' scripts and no crash in the runs without mods
      that pressed keys only.

Found and fixed during these runs:

- A dog in its spawn effect is shielded, so a skip right after the dogs came
  could find nothing to kill, leave out `last_ai_down`, and the dog round never
  ended. The skip now waits for an enemy it can kill, sends `last_ai_down` with
  it and kills the rest as their shields drop.
- The end screen's ROUNDS line sat on the game's scoreboard; it is above it now.
- With your stack the random loadout gave one gun a round, Ray Gun Mark II and
  the Thundergun in turn: DS4C orders the box's own pick so that its first gun
  is always the same, and the second pick matched the first. The two guns are
  now drawn evenly from the guns the box would give the player.
- A skip that had to wait more than 30 s for enemies left the votes counted with
  no skip; the votes are cleared then, with a notice to vote again.

One run without mods that read the pause menu through Lua from the console
closed silently in the match (no crash dump), as the crashed stack runs of stage
2 did; the runs after it press keys only.

Not tested yet: everything that needs a second player (votes, joining, a
self-revive while others are up), a map other than Kino der Toten, and rounds
reached by playing past 255.

### Zombies round settings, stage 4 (2026-11-07)

DESIGN.md 7.9 stage 4 (smart zombies), on `launcher/round-system`. Run by Claude
with your go-ahead ("Yes I'm not playing") on Kino der Toten, through
`C:\Users\Ginld\XercesDev\bench\20261107-rounds\stage4-run.ps1`: twice without
mods and once with your seven-mod stack (PEGASUS on top). Settings on as
launcher dvars: Round HUD and Smart Zombies. A test-only game script (`probe7\`,
copied into the worktree's `data\` for each run and removed after it) kept the
test player standing (it could not be hurt and did not move or shoot), moved the
game to round 30, counted for 60 s the zombies that came into the play area and
how many were smart (killing each once counted, so more came), then made every
new zombie smart as after round 999's Keep Going, and every 3 s reported what
the smart zombies were doing. No keys were pressed in the match. Captures are of
the whole screen (`capture-dpi.ps1`).

- [x] Lobby, without mods: the ROUNDS tab has Smart Zombies after Round Skips
      and Rounds to 999.
- [x] The share, rounds 30 and 31 (109 and 110 in 1,000): 0 of 24 in the first
      run (it killed nothing, so no more came), 22 of 152 with your stack and 14
      of 105 in the second run without mods: 36 of 281, 12.8%.
- [x] Every new zombie smart: 41 of 41 and 46 of 46 without mods, 63 of 63 with
      your stack, of all three kinds (19/5/17, 15/12/19 and 16/25/22 pack
      hunters, burst sprinters and ambushers).
- [x] What they did, from the reports: pack hunters held back (up to two at a
      time waiting) and then charged and reached the player; burst sprinters
      switched between walking and sprinting every few seconds; ambushers went
      to their spot and held there, then all struck (sprinting) and reached the
      player. One glow per smart zombie alive (24 of 24, 32 of 32), gone within
      a few seconds of the zombie.
- [x] The glows on screen: yellow, green and blue power-up glows on the zombies'
      heads. They light the whole zombie, and 24 smart zombies crowding a player
      who stands still tint the screen (KNOWN_ISSUES).
- [x] The notice at round 30: first it read "SMART ZOMBIES: (1, 1, 0) HUNT IN
      PACKS, (0, 1, 0) SPRINT IN BURSTS, (0, 0, 1) LIE IN WAIT": the compiler
      expands shared.gsh's colour macros inside strings. The colour words are
      now split in the script, and the second run without mods showed "SMART
      ZOMBIES: YELLOW HUNT IN PACKS, GREEN SPRINT IN BURSTS, BLUE LIE IN WAIT".
      No other string in the script holds a macro name (checked with a script).
- [x] No script error from Xerces' scripts or the probe in any run; none at all
      without mods. With your stack the console has DS4C's usual ones.
- [x] The same run with your stack on `launcher/integrate-beta7` (beta.6 with
      the MW4 Shipment memory fix and stages 1 to 4 merged): 15 of 167 smart at
      rounds 30 and 31, 62 of 62 after (24/20/18), no crash, no script error
      from Xerces' scripts, and the game log's `xerces.db_pool` showed the
      larger streamed-texture pool (30720 MiB) with Larger zone memory on.

Not tested yet: a player who moves and shoots, co-op (the pack's choice of
player), a Monkey Bomb or other point of interest drawing smart zombies, a
turned zombie, maps other than Kino der Toten, and rounds reached by playing.

### Friends, stage 1 (2026-11-07)

DESIGN.md 10.11, on `launcher/friends` (client) and `launcher/friends-backend`
(the backend's `GET /v1/me/address`). Run by Claude on one PC: two launchers,
each with its own home (`C:\Users\Ginld\XercesDev\bench\20261107-friends`, homes
A and B), both `--dry-launch` so nothing started the game, driven by check
scripts (`friends-run.ps1`, `friends-run2.ps1`), against the backend run on this
PC (`scratch\friends-local\run-backend.ps1`: 127.0.0.1:8090, throwaway keys, a
scratch SQLite file; `XERCES_CRASH_URL` points the launchers at it). Two test
accounts made there, alicetest (A) and bobtest (B). The live backend was not
used.

- [x] Unit tests: the saved sign-in is DPAPI-encrypted (neither the token nor
      the name is in the file) and read back; a damaged file is no sign-in;
      usernames and passwords are checked as the backend does; the invite text
      and its checks; sign-in, sign-up and their errors; an old access token is
      renewed and the refresh token rotates; a 401 gets one fresh token, then
      the sign-in ends; the backend's list and settings shapes; ids and invites
      refused before anything is sent; the socket's messages. 560 in all pass.
      Backend: 12 new tests for `/v1/me/address` (433 in all pass).
- [x] Sign-up on both, then A sends B a request: B's launcher hears it at once
      (a footer line, the request under Friend requests); B accepts and A hears
      that ("bobtest is now your friend" style line); both list the other as
      "Online in Xerces" with the dot lit.
- [x] An invite from B to A (through the API, as B's launcher sends it, with a
      documentation address): A shows "bobtest invited you to Zombies" under
      Invites with Join and Decline, and the footer says so. Join started the
      dry launch with `+set xerces_join <address>` (the log hides the address)
      and the invite left the list.
- [x] Privacy: A set "Who sees you online" to Nobody; B showed A as Offline. Set
      back to Friends, B showed A online again (second run).
- [x] A restart: both launchers came back signed in from `account.dat`, with
      their lists and presence.
- [x] Block (after its dialog): B left A's friends and showed under Blocked;
      Unblock removed it there (a block also ends the friendship, as the backend
      does). Sign out (after its dialog): back to the sign-in form, the socket
      closed, and `account.dat` deleted.
- [x] B, started after the block, listed no friends.

Found and fixed: B still listed A after A blocked B, until B's lists were read
again (the backend tells nobody they were removed or blocked); the launcher now
reads the lists every 60 s. That refresh itself was not watched in a run.

With the game, run by Claude with your go-ahead ("Run it"), the same local
backend, the game started under home A from the worktree build with mods off
(`game-run.ps1`, `game-run2.ps1`; keys only to the game window):

- [x] Hosting: in the Zombies lobby, `friends_open` (the DIRECT CONNECT button's
      command; the router answered no UPnP, as at home). Launcher A saw the open
      lobby within seconds ("Your lobby is open ...", Invite next to bobtest),
      and launcher B showed "alicetest · Playing Zombies · lobby open". Invite
      answered "Xerces could not tell your internet address: The server cannot
      tell your public address.": right for a backend on this PC, which sees
      127.0.0.1. The live backend sees the real address.
- [x] The pipe methods through `xerces_cli` (new `host`, `join`, `notify`): at
      the main menu `host` said not open, port 27017, nothing mapped; `notify`
      showed the toast; `join --address 203.0.113.7:27017` showed the game's
      "Connecting 203.0.113.7:27017" toast, then "Connect failed: No response"
      in the console (nothing answers at that address).
- [x] An invite from bobtest while A's game ran: A's launcher had the game show
      it ("the game showed the invite" in its log, the toast seen in a capture),
      and Join had the running game connect (`xerces.join.invite` in the game
      log, the same connect).
- [x] The toast's text: the game shows about 36 characters of title and 26 of
      text, one line each. The first text ("bobtest invited you to Zombies. Join
      from Friends in Xerces.") was cut; now the title is "Invite from" and the
      name, the text "Zombies, join in Xerces". The longest, a 24-character name
      and Multiplayer, fits (capture).

Not tested yet: an invite reaching a friend on another PC and their join into a
real lobby, the address from UPnP or from the live backend, the live backend and
its socket over Railway's edge, and a mod stack.

### Friends, stages 2 to 4 (2026-11-07)

DESIGN.md 10.12, on `launcher/friends` and `launcher/friends-backend`. The same
bench, homes and local backend as stage 1 (now with migration 0004).

- [x] Backend: `tests/test_links.py` (links, the share page, knocks, invites
      allowed by an accepted knock, declined and blocked knocks kept quiet,
      export, rate limits, the punch push with IPv4 and IPv6, an accepted invite
      lasting an hour); 454 tests pass.
- [x] Unit tests (566 pass): tickets in the host's guard (any of several keys;
      none lets nobody in), session text 2, link and pack codes in every pasted
      form, the new calls and pushes against a fake backend, the invite pack
      check (by id, by Workshop folder, a mods folder that only looks like one),
      the ticket on the command line and redacted in the log.
- [x] Two launchers, no game (`friends-run4.ps1`): A made a link; the share page
      answered 200 with the `xerces://join/<code>` button and its own CSP; a
      second start of Xerces for home B with `xerces://join/<code>` handed it to
      B's open window, which asked "Ask to join?" naming alicetest and Zombies;
      Ask to join knocked, A showed "Asking to join: bobtest · Zombies" and the
      footer line, and Ignore declined it (the backend's knock: declined). "Who
      can join" switched and was saved.
- [x] The joiner (`friends-run5.ps1`, invites from alicetest sent through the
      API as a host's launcher sends them, `craft_invites.py`): an invite whose
      pack names a mod that is nowhere showed "Joining alicetest's lobby", the
      mod, and "not on the Workshop"; Cancel ended it. The next invite from
      alicetest, while B followed her, was taken at once ("following a host's
      invite"); every mod was here, and the dry launch started with her one mod
      (`fs_game 2804432292`, not B's seven) and
      `+set xerces_join_ticket <address>` (the ticket redacted).
- [x] The host game (`game-run3.ps1`, mods off, Zombies lobby open): `host` said
      `main_menu` and `invite_only`; a punch at the main menu was refused (the
      front end counts as a running server, so `punch.cpp` checks for a lobby or
      a match); a stand-in joiner (`joiner_check.py`, the join check over UDP
      from 127.0.0.1) got `open` with invite-only off, then `wrong` without the
      ticket and `ok` with it once `guests` set invite-only and the ticket; a
      bad ticket was refused; a punch to this PC's LAN address started
      (`xerces.punch.start`), one to 127.0.0.1 was refused; launcher A, signed
      in, turned invite-only on by itself (`join.invite_only`); `quit` closed
      the game in 4 s with a clean exit in the journal.

Not tested yet: the punch's packets themselves (a listener on the LAN address
would have asked the firewall), a real joiner's game answering with a ticket,
two PCs behind two routers, the restart into the host's mods, Rejoin after a
real crash, a host's game restarting with its last lobby's players invited
again, the Workshop pages opening, "Open invite links in Xerces" (it writes
HKCU, so it was left alone), and the live backend.

Discord (`discord-run1.ps1`, launcher A, no game, with the Discord app running
on this PC):

- [x] Show on Discord, turned on: `launcher.discord.ready` (connected to the
      Discord app), and `HKCU\Software\Classes\discord-1568827390010859623`
      written with `"<Xerces.exe>" --home "<home>"`. Turned off: disconnected,
      the key gone again, and `friends_discord` saved as false.
- [x] Unit tests: the join secret `xerces1 <code>` and nothing else taken back
      (567 pass).

Not tested yet: the presence on a profile with the game running, and Ask to Join
between two Discord accounts.

### Rounds, freezes and Steam downloads (2026-11-08)

After a session with a friend on beta.10 (DESIGN.md 7.9, 9.2; CRASH_REPORTS.md).
Bench: `C:\Users\Ginld\XercesDev\bench\20261108-rounds` (`round8-run.ps1` with
the probe `probe8`, `freeze-run.ps1`), the test home of 20261106-modlabels, the
player's own Workshop mods and maps.

- [x] The friend session's logs: invites, tickets and the mods check worked; the
      two drops were joining SHOOT HAUS (`zm_shoothouse`), as it loaded; the
      freeze was in BACKROOMZ while hosting, the main thread waiting on a lock
      in the script VM (`ScrVar_ReleaseValue`, from `Scr_MakeArray`), the holder
      unknown (only the main thread was recorded then).
- [x] With all 12 of the player's mods on SHOOT HAUS (46 zones) the load hung:
      the main thread waited in `DB_FindXAssetHeader` for an asset that never
      came (likely past the engine's 63 zone slots, which Xerces cannot check
      for this build). With 9 of them (PEGASUS, DS4C, Ascendance, Solo Easter
      Eggs, Girls' Frontline, Neptunia, Classic FOV, Avali, im_oxo QOL), the
      probe on SHOOT HAUS (start round 20), BACKROOMZ (5) and RUST V5 (1): the
      round and speed (round x 4) as set; the hidden text and HUD 2 s after the
      spawn, with the starting round as the sixth number; `GetLUIMenuData` on a
      closed menu answers undefined; both random-loadout guns Pack-a-Punched;
      four coin flips asked, three run, the fourth told "Three coin flips a
      round"; down, jump held 2.5 s, up, the self-revive used at that round; the
      hidden text closed by the probe was open again at the next round. Captures
      show the HUD, round 20 and HOLD JUMP OR USE TO REVIVE YOURSELF. RUST's own
      `_zm_weapons` raises a non-terminal script error ("'undefined' and 'bo6'")
      from the upgrade; the guns were upgraded.
- [x] A freeze on purpose (mods off, a console Lua loop at the main menu): at 30
      s `freeze-<time>.txt` (the main thread's stack, then 27 other threads,
      named where they have names) and a 618 KB `freeze-<time>.dmp`; the game
      closed itself at 61 s (`exception.freeze.closed`).
- [x] `xerces_cli workshop get 2804432292` (installed already): Steam's API
      started as Black Ops III, "ready", then "installed". Steam's library
      printed the Steam id on stderr, which the callers throw away.
- [x] Unit tests: the helper's progress lines, Steam's item state flags and id
      lists; the join check's "Get ... through Steam now?" (570 pass).

Not tested yet: a download of an item not yet installed (it subscribes the
account), the join panel and the in-game Workshop menu downloading for real, a
freeze in a launcher-started game sending its report, two PCs (whether the later
menus keep SHOOT HAUS joins in sync), and the controller's jump.

### Larger zone memory: 4, 6 and 8 GB (2026-11-08)

DESIGN.md 5.4 (zone memory). Settings > Launch has Off, 4, 6 and 8 GB, written
as `-xerces-db-pool` and, for 6 and 8, `-xerces-db-pool-6gb` or `-8gb` (the
launcher's flag field takes no `=`). The page budget follows the pool (pool plus
4 GiB: 8, 10 and 12 GiB, under the 16 GiB its page table holds), and the
planner's memory checks use the chosen size. Bench:
`C:\Users\Ginld\XercesDev\bench\20261108-upscale` (`town-run.ps1`,
`probe-run-flags.ps1`, `launcher-row.txt`), the test home of 20261106-modlabels,
the player's mod stack (DS4C, Ascendance, Girls' Frontline, im_oxo QOL, Classic
Viewmodel FOV) on Town Reimagined (`zm_town`, 3.4 GB).

- [x] The game: with 4 GB the DB main pool is 4096 MiB and the page budget 8192
      MiB (as before); with `-xerces-db-pool-6gb` 6144 and 10240 MiB; with
      `-xerces-db-pool-8gb` 8192 and 12288 MiB; each reads back the size it was
      given, the streamed-texture pool is 30720 MiB in all three, and Town
      Reimagined loaded in about 40 s each time and held 2282 MiB of the pool
      (`xerces.db_pool`, `xerces.db_pool.usage`).
- [x] The player's own log of the same stack: the pool peaked at 2726 MiB over
      four maps in a session (Digital Circus, Town Reimagined, Cheese Cube,
      Shadows of Evil), so 4 GB was not full; 6 and 8 GB matter only for heavier
      stacks (the BACKROOMZ, Super Mario 64 and MW4 Shipment class).
- [x] The launcher (a copy of the test home, `--view settings`, scripted
      clicks): the Launch section shows Off, 4 GB, 6 GB and 8 GB; each click
      writes the flags the choice means (8 GB:
      `-borderless -xerces-db-pool     -xerces-db-pool-8gb`) and the "The game
      starts with" line shows them.
- [x] Unit tests: the sizes, the budget for each (never past 16 GiB or a pool's
      page index), the detours' target sizes, the flag parsing (the last size
      wins, a size alone turns nothing on, other spellings are 4 GiB), the
      settings' flags round trip, the report against the chosen size, and the
      planner's pool size (584 pass).

Not tested yet: a stack that needs more than 4 GiB (nothing on this PC does),
the culprit search's handling of a 6 or 8 GB choice beyond the unit tests, and
the diagnostics screen's wording (it still says "On (-xerces-db-pool)").

### The 12-mod stack on 19 Workshop maps (2026-11-08)

Bench: `C:\Users\Ginld\XercesDev\bench\20261108-upscale` (`maps-check.ps1`,
`run-maps\`), the test home of 20261106-modlabels, 4 GB zone memory. Per map:
the level loads, 30 s of walking with the frame probe, screenshots, console.

- [x] The 12 mods (PEGASUS, DS4C, Ascendance, Solo EE, Girls' Frontline, Girls'
      Frontline 2.0, Neptunia, Bodycam, Classic FOV, Avali, QOL, SOE Unlocked)
      hang on every level (main thread in `DB_FindXAssetHeader`, even on the 770
      MB Buried Challenge). Removing Girls' Frontline 2.0 loads it; removing
      Bodycam or SOE Unlocked does not. Girls' Frontline and its 2.0 are two
      versions of one mod (same folder name `gfl`): use one.
- [x] The 11 mods on all 19 maps: 17 load and run 30 s with no crash, freeze or
      memory error (zone pool peak 1.5 to 3.3 GiB of 4); frame times 168 to 257
      FPS, p99 5 to 10.6 ms, no hitch over 33 ms on 15 of them.
- [x] Big Bang Tower ended the game with no message at about 40 s with DS4C +
      Ascendance (not with either alone, nor with other mod groups): the map's
      own UI Lua replacing the mods' (`xerces_map_ui_first`). With it off the
      full stack loads, as do RUST, Town Reimagined, Super Mario 64, Digital
      Circus, Wizard Castle and Shoot House. Default is now off.
- [x] DIE RISE does not load with DS4C (alone too) or with another mod in the
      stack: "Server Disconnected - Clientfield Mismatch"; PEGASUS alone loads.
      Not fixed (script-level).
- [x] Der Riese: 2,384 to 2,953 script errors a frame-rate's worth
      (`_zm_perk_razor.csc`, "type undefined is not an int") with the mods and
      without them; 10 frames over 100 ms. The map's own bug.
- [x] Script errors on every map (about 110 to 390): DS4C's `live_xp.gsc` ("LUI
      event name is not precached") and `fr_commands.gsc`.

Not tested: mod features beyond loading (menus, characters, perks), longer play,
two PCs. Character mods replace the default models (same asset names); new
operator slots are not built.

### Record frame times (2026-11-08)

Settings > Display > "Record frame times" adds `-xerces-record-frames`; the
launch then gives the game `XERCES_BENCH=1`, `XERCES_BENCH_AUTO=ingame:20:600`
and `XERCES_BENCH_OUT=<client folder>ench` (the probe refuses AppData and the
game's folder).

- [x] A launch from the launcher (test home, vanilla): the game log has
      `perf.bench.armed`, "Frame probe on; runs go to <client folder>ench". The
      setting is saved as `-xerces-record-frames` in the flags.
- [x] Unit test: the preview sees the flag (585 pass).

Not tested yet: a full ten-minute capture from the launcher (the probe itself
was used for the runs below), and what the files look like when the player
changes maps during the run.

### Town Reimagined stutter, first look (2026-11-08)

The player reported stutter on Town Reimagined with their stack, and DLSS "maybe
a little more stuttery than SMAA". Two runs of `town-run.ps1 -Segments` (the
frame probe, XERCES_BENCH=1; `nvidia-smi` and the process's RAM sampled
alongside; 4K, Scene Resolution 70%, Filmic SMAA T2x or DLSS Quality, 4 GB zone
memory), each with 40 s of walking at the spawn; then the player went down at
about 1:40 and the game left the level (cheats cannot be switched on here, so
`god` is refused).

- [x] Filmic SMAA T2x, both runs: median 4.0 ms (about 250 FPS), p99 6.0 to 6.5
      ms, longest frame 9.6 and 11.0 ms, no frame over 12 ms in 43 s each.
- [x] DLSS Quality, the in-level part of both runs (about 18 s each): median 5.5
      to 6.0 ms (about 165 FPS), p99 8.0 ms in the first run; two frames of 126
      ms at 13.7 s and 14.3 s (first run), then the player went down and the
      level ended (149 and 717 ms; 675 ms in the second run). The frames after
      the exit are the front end's, not the level's.
- [x] The GPU was at 98 to 99% in both (the game is GPU-bound at 4K, 70%), VRAM
      10.8 GiB of 16 in both, the game's working set 11 GB (32 GB PC). DLSS
      costs about 2 ms a frame here (4.0 to 6.0 ms); the card sat at its
      software power cap with DLSS.
- [x] The console of the first run: 1,824 script errors in 3.5 minutes, nearly
      all while the map and mods loaded; only 4 and 5 during the 40 s walks.
      They name `scripts/zm/explosive_zomb.gsc` (in the mods' `zm_mod` zone and
      also in `zm_town`: the engine says "Redundant scriptparsetree asset ...")
      and DS4C's `fr_commands.gsc` and `hud_util_shared.gsc`.

Not found yet: the player's stutter. Near the spawn the level was smooth in both
modes. It may be elsewhere or later (streaming while moving through the map,
many zombies, a round's script work), which a capture in real play would show.

### Town Reimagined's 140 ms frames: the streamer's sort (2026-11-12)

What the main thread waits for in the long frames of the player's two recordings
(`C:\Games\xerces-0.1.0-beta.17\...\bench\20261108-171142-auto-ingame.*`, DLSS, and
`C:\Games\xerces-0.1.0-beta.20\...\bench\20261112-011942-auto-ingame.*`, FSR 1), and the
fix (DESIGN.md 12.4 item 7, `src/client/xerces/stream_sort.cpp`). Bench:
`C:\Users\Ginld\XercesDev\bench\20261112-town` (`town-sort.ps1`, a folder per run with
the probe's files, the game log and two captures); the dump of 2026-11-04 read with
`func_of.py`, `llvm-objdump` and `dump-xrefs.ps1`.

From the recordings and the game's code:

- [x] The long frames are 125 to 143 ms every time (355 s recording: 561 of them, none in
      the first 30 s, about 3 a second from 3 minutes on; they grow from 128 to 143 ms).
      Between two of them lie 14, 25, 36 or 47 frames in one recording (the 240 cap,
      4.2 ms a frame) and 13, 20, 27 or 34 in the other (DLSS, 6.5 ms): steps of about
      46 ms in both, a cadence of time and not of frames.
- [x] The wait is no lock and no event. The three system frames are Sleep(0): the main
      thread spins in the job system's "wait until this kind of job has none queued or
      running" (0x14000A420, reached through 0x142257F60), asked for the job
      `r_stream_sort` (its module at 0x1432FAED0) by the streamer's "end the query"
      (0x141D07940), which every frame's streamer update (0x141D07480, then 0x141D09A70)
      calls before it starts the next query.
- [x] The job (0x141D0B040, then 0x141D08520) puts two lists in order: the image parts
      wanted and not loaded, and the image parts loaded (one array of 0x30040 entries at
      0x150904100, filled from both ends). When the first list is as long as it was at
      the last sort it calls an insertion sort (0x141D01870, the order of
      0x141D036B0 inlined: parts whose importance is over 0.75 first, then by file and
      offset); otherwise `std::sort` (0x141D02C30).
- [x] The time is in that insertion sort. In 20 of the 591 stalls the sampler caught in
      the second recording the main thread took the job itself: every sample of those
      frames, 12 or 13 in a row over 100 ms, is inside 0x141D01870 called from
      0x141D08865 (the list of loaded parts). In the others a worker runs the same job
      and the main thread waits.
- [x] The streamer works in rounds (4 + 2 + 1 steps once a level has loaded, a counter
      at 0x150AB2668) and queues the sort with a round's last step. It sorts the lists
      whole only while it takes the level to be still loading (a flag at 0x150AB26BE is
      0, or a frame had no view) and its lists changed (0x150AB271C); once everything
      wanted at the start is loaded it takes a cheaper path (0x141D0AB20). The loading
      state is set again when a newly loaded zone of certain kinds is found
      (0x141424D20 calls 0x141CFE3A0 with the zone's name).

In the game (the player's eight mods, `zm_town`, 4K, their launch flags
`-xerces-precise-limiter -xerces-db-pool -xerces-db-pool-8gb` and `com_maxfps 240` in the
later runs; a scripted player that walks, turns and, in one run, fires and aims):

- [x] The game's own sort, timed (`XERCES_STREAM_SORT=engine`): while the level loads the
      lists are sorted up to 30 times in 10 s, in at most 0.8 ms (the loaded list grows to
      16,000 parts, the wanted one starts at 17,900); afterwards no whole sort and no
      long frame until the player is down and the level ends (90 to 120 s in), in every
      run. There the lists are sorted a few times more: a loaded list of 14,200 parts
      with 10 stretches out of order already costs the game's sort 1.5 ms, one with 137
      stretches 2.0 ms.
- [x] The worst case on purpose (`XERCES_STREAM_SORT_TEST=reverse`: the loaded list turned
      back to front before each sort). The game's sort: 42 ms for 7,700 parts, 151 ms for
      13,800, 174 ms for 16,000, 227 ms for 19,200. So the player's 125 to 143 ms is what
      this sort needs for a list of that length in no order at all.
- [x] The same with Xerces' merge: 2.0 ms for 14,600 parts (14,287 stretches), 1.3 ms for
      16,200; `xerces.stream_sort.far` says so in the log ("far out of order 36 times in
      the last minute; the longest sort took 2.0 ms"); no frame over 8.5 ms in the 75 s
      recorded.
- [x] The merge as shipped, no test switch: the same sorts as the game's during the load
      (0.3 ms at most for 15,300 parts), the two at the level's end 0.34 and 0.27 ms
      where the game's took 1.5 and 2.0; in the 90 s before the level ends the median
      frame is 4.5 ms at the 240 cap, p99 6.6 ms, the longest 12 ms (the game's sort
      with the same flags: 4.5, 7.0 and 13 ms); textures look as before in the
      captures; no new warning or error in the log.
- [x] `xerces_cli sig check --dump`: 19 of 19 signatures, the three new ones at
      0x1D0883C, 0x1D08865 and 0x1D036B0. Unit tests: the merge leaves what a stable
      insertion sort leaves (random lists up to 4,000 items), one comparison an item for
      a list in order, at most items x (passes + 1) for the others, the registry
      entries (604 pass).

- [ ] Not reproduced: the state the player's game was in. In none of seven scripted
      sessions did the game call the insertion sort between the load and the level's
      end: with the player's flags and frame cap, while walking, firing, with the pause
      menu open, or with the streamer held in its loading state
      (`XERCES_STREAM_SORT_TEST=loading`). What keeps the player's lists changing while
      the wanted list keeps its length, from the first minute on, is not known:
      `r_stream` and `r_streamClear` cannot be set from the console, and the scripted
      player is down after 90 to 120 s. The fix does not depend on it (the two calls
      are the only ones of that sort), but it has not been seen in the player's own
      play: after a session on Town Reimagined the log's `xerces.stream_sort.far` lines
      say how often the lists were far out of order and what the sorts cost.
- [ ] Not tried: other maps and Multiplayer or Campaign with the merge (the code is the
      same for all), a game build other than 0x6531394.

### AMD FSR 1 upscaler (2026-11-08)

DESIGN.md 13.3 (Phase A). Bench:
`C:\Users\Ginld\XercesDev\bench\20261108-upscale` (`trace-run.ps1`,
`fsr-run.ps1`, `ui-run.ps1`, `ui-video-run.ps1`, `func_of.py`), the test home of
20261106-modlabels, mods off, Shadows of Evil (`zm_zod`), 4K, the player's own
Scene Resolution Ratio 80% and Filmic SMAA T2x. The game's `players\config.ini`
was copied before each run and put back after.

- [x] The frame tracer (`XERCES_GFX_TRACE=1`): device methods shared with the
      WARP device, no deferred contexts; a frame of 2,512 events. The scene ends
      in a 3072x1728 R11G11B10 texture (the T2x resolve's output) and one
      full-screen draw (`r.final_composite`, 0x1C617E0, reached by a tail jump
      from the post-fx function) stretches it into the 4K back buffer with a
      bilinear clamp sampler; its pixel shader scales by 2^-15, applies the
      display curve from `PostFxCBuffer` and the brightness offset. The quad's
      UVs are 0 to 1 (a 1.0 constant in the code). The HUD is drawn after it.
- [x] `sigmaker.py` made the signature (one match in the first `.text`); the
      address is not in `integrity.hpp`'s list.
- [x] In the game: the log has `xerces.upscale.installed`, then with
      `xerces_upscaler 1` `xerces.upscale.active` "AMD FSR 1 upscales 3072x1728
      to 3840x2160, sharpness 8/10". A traced frame shows, after the game's
      composite, the composite's own shader 1:1 into a 3072x1728 texture, EASU
      to 4K and RCAS into the back buffer, then the HUD as before.
- [x] Full-size captures of the same view: FSR 1 is visibly sharper than the
      game's stretch at 80% (bricks, window frames, the HOTEL sign, a riveted
      panel), same colours and brightness; sharpness 0 (EASU alone) lies
      between; the HUD text is the same in all of them. 100% with sharpening
      alone shows no artefacts. The FPS counter (CPU-bound here) read 242 to 253
      either way. The player was downed before the 50% FSR capture, so 50% was
      only seen with the game's stretch.
- [x] Menus (front end): XERCES SETTINGS' GRAPHICS tab shows Upscaler (Off) and
      Upscaler Sharpness (80%) with their descriptions; Graphics > Video's list
      builds 15 rows with the two after Scene Resolution Ratio; no Lua errors.
- [x] Unit tests: the passes chosen per size and sharpness, the stops, the EASU
      constants against `FsrEasuCon`, the registry entry and its entry bytes
      (575 pass). `tools/shaders/build_shaders.py --check`: the headers match.

Not tested yet: a 50% capture with FSR 1 (the player went down first), other
anti-aliasing modes (FXAA, SMAA 1x, no anti-aliasing) and HDR or other Display
Gamma choices, a measured GPU cost, mods, Multiplayer and Campaign maps,
changing the options through the menus and Apply, and a window resize while it
runs.

### NVIDIA DLSS and DLAA (2026-11-08)

DESIGN.md 13.4 (Phase B). Bench:
`C:\Users\Ginld\XercesDev\bench\20261108-upscale` (`build-dlss.ps1`,
`dlss-run.ps1`, `dlss-sign-run.ps1`, `dlss-motion*-run.ps1`,
`dlss-final-run.ps1`, `shift.py`, `temporal-notes.md`), the same test home and
map, the RTX 5070 Ti, NVIDIA's DLSS SDK 310.9.16 (the release's git hashes
checked, `nvngx_dlss.dll` signed by NVIDIA). The player's `boiii_players` files
(`config.ini`, `user\config.cfg`, the bindings) are copied before the later runs
and put back after; three test values an earlier run left in `user\config.cfg`
(`cl_yawspeed 50`, `xerces_upscaler 2`, `xerces_upscalerSharpness 0`) were taken
out by hand.

- [x] `xerces_dlss_check.exe`: "DLSS available", one DLAA frame (1920x1080) and
      one Quality frame (2560x1440 to 3840x2160) ran.
- [x] In the game at 80% (3072x1728 to 4K): "DLSS: Quality, 3072x1728 to
      3840x2160, 13 jitter positions"; at 100%: "DLSS: DLAA, 3840x2160 (DLAA), 8
      jitter positions". The two jitter detours report the same frame number,
      and the offset runs on the resolve's thread.
- [x] The jitter's signs: with the jitter held at (0.4, 0.3), phase correlation
      against the game's own picture gives about +1 pixel in x when x is flipped
      and -0.6 to -0.9 in y when y is not; only (x, -y) gives about 0 in both,
      which is now the default.
- [x] Motion vectors: the geometry pass's shader encodes current minus previous
      position the way the decode undoes it; a logged mean at the screen's
      centre is 0 standing still, -40 turning left and +40 turning right (the
      arrow-key turn is fast enough to hit the 40-pixel clamp, so the unclamped
      range was not seen).
- [x] Captures: DLSS at 80% and DLAA at 100% look right (colours, HUD, no
      artefacts); DLAA lines up with Filmic SMAA T2x within 0.03 pixels and its
      edges are a little cleaner. Turning, the edges stay clean. The FPS
      counter, CPU-bound here, read about 178 with DLSS at 80% against 258 with
      the game's own anti-aliasing.
- [x] Menus: with the module there, Graphics > Video's Upscaler lists "Off",
      "AMD FSR 1" and "NVIDIA DLSS / DLAA"; XERCES SETTINGS opens.
- [x] Unit tests: the DLSS plan, Halton values, the jitter positions and their
      range, the three registry entries and what the client reads from the
      projection function's code (578 pass).

Not tested yet: motion between 1 and 40 pixels a frame (ghosting), fast turns
past the clamp at length, killcams and map changes (resets), other maps and
mods, Multiplayer and Campaign, DLSS below 80% (Balanced, Performance), a GPU
time measured on its own, changing the option through the menus, and a game
without the module (the option must not show).

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
`-Theme contrast` or `-Language pseudo`; `-Scale 1.5` fixes the UI scale and
`-Exe` runs another build, such as the previous one for before and after). A
script plays timed steps, the first at 1 s: `+500 key down`, `+300 pad a`,
`+200 view mods`, `+100 click launch-vanilla`, `+100 type text`,
`+100 focus search`, `+100 shot C:\path\screen.png` (a PNG of the next frame),
`+100 front off` (act as if another window were in front; `on`, `auto`),
`+100 minimize`, `+100 activity` (log `launcher.activity` for the span since the
last line), `+100 quit`. The log is
`<home>\logs\<date>-<time>-launcher-<pid>.jsonl`.

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
- [x] (2026-11-05, launcher/integrate-1: launcher-idle, frame-limiter and
      flip-model merged on xerces-main; dry launches only, home
      `C:\Users\Ginld\XercesHome-agent-integrate`, script
      `C:\Users\Ginld\XercesDev\scratch\integrate\dry-check.ps1`) On a
      hand-written journal of a modded crash in a match with larger zone memory,
      the precise FPS limit and flip-model presentation on, Home's safe-mode box
      showed the phase's sentence for experimental settings and one sentence per
      setting. "Find the culprit" started a search with the three settings in
      `culprit.json` (`settings` list, `db_pool` `untested`); test 1 named the
      three as left out, and its dry launch logged the command without any of
      the three flags. With the step set to `culprit`, Home said "Found it: the
      experimental settings that were on: ..." with "Turn them all off", which
      removed the three flags from `launcher.json` and ended the search; with
      two on (`-windowed` kept), "Turn both off" removed only those two.
      Settings > Display shows both toggles with their help texts and each saves
      its flag. A pad's Up from the Home tab focused the footer's About link,
      and A opened About. check-screens (dark, light, pseudo) and check-idle
      pass on the merged build; `xerces_cli plan apply --dry-launch` printed a
      note for each of the three settings. After the review (script
      `C:\Users\Ginld\XercesDev\scratch\integrate\fix-check.ps1`, also in the
      pseudo-locale): with only larger zone memory on, a crash in the menus gave
      xerces-main's sentence word for word, and flip-model presentation alone (a
      crash in a match) its own sentence naming Settings > Display; a
      `culprit.json` whose `settings` named `xerces-other` was not continued
      (Home offered "Find the culprit" again).
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

DESIGN.md 12.6 and 12.7 have the numbers and what changed. Nothing here starts
the game: `Xerces.exe` runs with `--dry-launch` only, and Workshop files are
only read.

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
  exit (frames presented and passes of the loop, `wakes`, in front, behind,
  minimized and while a game runs, and the resolve totals). Since the idle work
  (DESIGN.md 12.7) `--seconds` no longer keeps the loop busy, so these counts
  are what a player's launcher draws.
- `tools\dev\bench\launcher-idle.ps1 -HomeDir <home> [-Exe <Xerces.exe>] [-Label before]`
  measures an idle launcher, one minute per state: in front, behind (a small
  empty window titled "Xerces idle check" takes the foreground), minimized, and
  with a stand-in game running (a sleeping `powershell.exe` announced in
  `<home>\run`, never the real game), behind and minimized; then the stand-in
  exits and it prints how long the launcher took to notice. About seven minutes;
  leave the PC alone meanwhile. It prints frames and passes per minute for each
  state the window was really in and saves them as JSON in `<home>\bench`.
- `tools\dev\launcher\check-idle.ps1 -HomeDir <home>` checks a built launcher in
  about a minute, from the `launcher.activity` line between two `activity` steps
  of a script: idle in front draws nothing; a focused text field blinks its
  caret in front but draws nothing behind another window; a screen shown while
  minimized draws nothing; behind a stand-in game (as above) nothing is drawn
  and the loop wakes about once a second; a transition in front draws on every
  pass while it runs. It prints one line per case and exits 1 when one fails.

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

Run on 2026-11-05 by Claude for the idle loop (DESIGN.md 12.7; dry launches
only, the game never started), the old loop being 843ba747's:

- [x] `launcher-idle.ps1`, frames a minute before and after: in front 552 and 0,
      behind 560 and 21 (the change of foreground; a second minute behind drew
      none), minimized 540 and 0, a stand-in game running with the launcher
      behind 542 and 4 (the restore), minimized 546 and 0. Passes a minute
      after: 553 in front, 570 behind, 538 minimized, 62 with the game running.
      The stand-in's exit was noticed after 851 ms (86 and 876 ms in two runs of
      the old loop).
- [x] `Xerces.exe --dry-launch --skip-boot --view home --seconds 15`: the old
      loop drew 66,714 frames (4,478 a second: Present did not wait, the display
      probably asleep), the new one 23 frames in 156 passes.
- [x] `check-screens.ps1`, also with `-Theme light` and `-Language pseudo`:
      every screen captured, no RmlUi warning or error.
- [x] A scripted walk (Home, Down, Mods, E, RB, Q, Q, Down, Down) with six
      `shot` steps: the six PNGs are byte for byte the same as the old loop's,
      which drew 38,895 frames for it; the new one drew 90 in 136 passes.
- [x] A `view mods` step while the window is minimized draws nothing (0 frames
      in 3.1 s, 38 passes; the old loop drew 8,366); restored, the desktop shows
      Mods, as with the old loop.
- [x] `m1-check.ps1 -Case vanilla` and `-Case modded` without `-Live` pass: the
      warm run (`--seconds 900`, closed once the scan is logged) and the run
      with `+3000 click` and `--seconds`, the way `map-check.ps1` drives the
      launcher. `map-check.ps1` itself was not run (it starts the game).

Run on 2026-11-05 by Claude after the review of the idle loop (DESIGN.md 12.7:
finely timed waits, frames paced whatever woke the loop, no caret behind another
window, `WM_SYSTIMER` no reason to draw; dry launches only, the game never
started), the old loop being fb165490's:

- [x] `xerces_tests`: 402 tests pass, 14 of them new (`launcher pacer`,
      `precise wait`).
- [x] `check-idle.ps1`: every case passes. In front idle 0 frames, 11 to 12
      passes a second; a caret in front 5 to 7 frames in 4 s; behind with a
      focused text field 0 frames; minimized with a screen shown 0 frames;
      behind a stand-in game with a focused text field 0 frames, 1.2 to 1.8
      passes a second (fb165490 drew 21 more frames in 12 s with the caret than
      without it, the reviewer's case); a transition in front 17 to 58 frames.
- [x] One 0.2 s transition (focus leaving Launch modded), three runs with and
      without it: fb165490 drew 11 frames for it in front and about 5 behind
      another window; the new loop 54 and 10. Present returned at once in these
      runs, so the 54 are the frame gap's limit, not the display's.
- [x] A 5 s run on Home in front: 41 frames with fb165490, 9 now (`WM_SYSTIMER`
      every 530 ms drew four frames each in the first seconds).
- [x] `check-screens.ps1`, also with `-Theme light` and `-Language pseudo`:
      every screen captured, no RmlUi warning or error.
- [x] A scripted walk (Home, Down, Mods, E, RB, Q, Q, Down, Down) with six
      `shot` steps: the six PNGs are byte for byte the same as fb165490's (130
      frames for it, the new loop 79).
- [x] `m1-check.ps1 -Case vanilla` and `-Case modded` without `-Live` pass.
      `map-check.ps1` was not run (it starts the game); it drives the launcher
      with `+3000 click` and `--seconds`, as m1-check's runs do.
- [ ] With you and the game: the launcher behind a match draws nothing (its
      `launcher.activity` lines in `<home>\logs`), it notices the game's exit at
      once (it waits on the process it started), and on a live display the
      screens' transitions stay smooth at the display's refresh rate (not seen
      here: the launcher stayed behind other windows, and Present returned at
      once).

### Launcher text shaping (HarfBuzz, 2026-11-05)

DESIGN.md 7.1 has the design and the numbers. `Xerces.exe` logs which font
engine it runs as `launcher.font.engine` (`harfbuzz` with its version, or
`freetype` in a build made without `third_party\harfbuzz`), with the language
tag and script the documents are shaped in. For before and after, keep the old
build next to the new one as `Xerces-old.exe` and capture both with the same
home and scale:

```
tools\dev\launcher\check-screens.ps1 -Out <dir>\before\100 -HomeDir C:\Users\Ginld\XercesHome-agent-harfbuzz -Scale 1.0 -Exe build\bin\x64\Release\Xerces-old.exe
tools\dev\launcher\check-screens.ps1 -Out <dir>\after\150 -HomeDir C:\Users\Ginld\XercesHome-agent-harfbuzz -Scale 1.5
```

Run on 2026-11-05 by Claude (dry launches only, home seeded from
`XercesHome-bench`). The captures are in
`C:\Users\Ginld\XercesDev\scratch\harfbuzz\captures`: `before\` and `after\`
with `100`, `150` (every screen) and `pseudo-100` (Home, Mods, Settings),
`after\light-100`, `after\contrast-150`, `after\about`, and `no-harfbuzz\100`
(Home and Packs from a build without HarfBuzz).

- [x] With HarfBuzz the launcher builds, the log says
      `"engine":"harfbuzz","harfbuzz":"15.0.0","lang":"en","script":"Latn"`, and
      every screen loads with no RmlUi warning or error at 100 and 150 percent,
      in the pseudo-locale and the light and high-contrast themes. Without
      HarfBuzz (a third-party folder holding only `rmlui` and `freetype`) it
      builds, logs `"engine":"freetype"` and draws Packs pixel for pixel as
      before (Home too, but for the scan time it shows).
- [x] The bundled fonts (Inter Tight's weights, IBM Plex Mono Regular and
      Medium) load and draw; accented letters in the pseudo-locale draw.
- [x] Kerning: "Yo" in Packs' "Your packs" (Inter Tight 700, 20 dp) closes by 2
      px at 100 percent and 3 px at 150 (the o's ink ends at x 56 before and 54
      after; 85 and 82 at 150); "Pa" in the Packs title and "ay" in Settings'
      "Display" by 1 px.
- [x] Nothing wraps, clips or moves: every changed pixel lies in a changed line
      of text; the pseudo-locale's overflows (the long tilde runs) are the same
      before and after.
- [x] Widths of 27 launcher strings at 100 and 150 percent (a scratch program on
      both engines): equal to RmlUi's FreeType engine except where kerning
      applies, 0 to 7 px narrower.
- [x] Startup, `tools\dev\bench\launcher-bench.ps1 -Runs 20`: first frame 209.9
      ms before and 212.4 after (medians; p90 220.1 and 222.1); with the two
      builds alternating, 12 starts each, 211.2 and 211.0. The first screen is
      loaded at 57.9 and 67.4 ms, Home is shown in 29.8 and 39.4 ms (see the
      review fixes below: now 60.2 and 27.7 ms).
- [x] About lists HarfBuzz 15.0.0 and RmlUi's HarfBuzz engine, and only in a
      build that has them (captured from both builds after the review, below);
      `data/xerces/licenses` holds the licence texts. Since the beta package
      (launcher/integrate-2) that folder is gone: the texts are in
      `THIRD_PARTY_NOTICES.txt` (`tools/release/notices.py`), which About opens.
- [ ] You looking at the text on your display, at your usual scale.
- [ ] A translation in a script other than Latin, or written right to left.

Review fixes, run the same evening by Claude (dry launches only, same home).
Captures in `C:\Users\Ginld\XercesDev\scratch\harfbuzz\fix\captures`: `head\`
(the build before these fixes, kept as `Xerces-head.exe`) and `new\` with `100`
and `150` (every screen and About) and `pseudo-100` (Home, Mods, Settings);
`no-harfbuzz\` (About, Home, Packs, Settings) and `old\` (Home, Packs, Settings,
from the build before HarfBuzz) at 100 and 150. The scratch programs and timing
scripts are next to them in `scratch\harfbuzz\fix`.

- [x] Text is never drawn wider than it is laid out.
      `tools\dev\launcher\font-width-check.ps1` drives the built engine as
      RmlUi's `ElementText` does, the line as the sum of its words measured one
      by one, then drawn whole, for all 752 launcher strings in Inter Tight 400
      to 900 and IBM Plex Mono Regular and Medium at 16 sizes from 12 to 120 px:
      0 of 96,256 lines drawn wider (built against the engine from before the
      fix: 4,647, by up to 5 px, exit code 1), and the whole-string width always
      equals the drawn width. Lines can be drawn up to a pixel or two narrower
      than laid out where Inter Tight kerns a comma or full stop against the
      space after it, which RmlUi measures in different words.
- [x] Layout is as before the fix: every box keeps its width (the scratch
      `widths.cpp` finds the laid-out widths unchanged for every string, weight
      and size it tried), and single words draw the same; letters after a word
      space move by up to a pixel. Against `head\`, Home at 100 percent changes
      only inside three lines of text (a word after a space, and the underline
      of a link that ends after one); nothing wraps or clips on any screen, and
      the pseudo-locale's overflows are unchanged.
- [x] One layout per document: the root's `lang` and the UI scale are set before
      the documents load, so each document is laid out and shaped once (a dry
      run logged `"lang":"en"` and the final scale on every document as it
      loaded; the check was then removed). Startup times are in the table below
      the list.
- [x] Memory, 3 s into a dry start on Mods, four starts each: working set 93.1
      to 93.4 MB with HarfBuzz against 88.0 to 88.2 MB without at 150 percent,
      private bytes 195 to 197 MB against 190 to 191 (90 to 91 and 86 to 88 MB
      at 100 percent). About 5 to 6 MB for HarfBuzz; DESIGN.md 7.1 says where.
- [x] A build without HarfBuzz (a third-party folder holding only `rmlui` and
      `freetype`) logs `"engine":"freetype"`, draws Home, Packs and Settings at
      100 and 150 percent pixel for pixel as the build before HarfBuzz, and its
      About has no HarfBuzz entries (`no-harfbuzz\100\about.png`,
      `no-harfbuzz\150\about.png`).
- [x] About with HarfBuzz names the Microsoft Universal Shaping Engine data
      (MIT) and the Unicode data (Unicode License V3) built in with it, and
      `data/xerces/licenses/HarfBuzz.txt` has HarfBuzz's `COPYING`, its
      `src/ms-use/COPYING` and the Unicode License V3 (`new\150\about.png`).
      Since the beta package these texts are HarfBuzz's entry in
      `THIRD_PARTY_NOTICES.txt` (with UCDN's and fasthash's headers and the
      copyright lines `COPYING` does not list, since the candidate's review
      fixes); `data/xerces/licenses` is gone.
- [x] Kerning seen in the captures: "Yo" in "Your packs" closes as before. Inter
      Tight also kerns some pairs apart: "rf" in "HarfBuzz" on About opens by
      0.64 px at 150 percent and is drawn a pixel wider than RmlUi's FreeType
      engine draws it, which reads like a gap next to FreeType's wide hinted
      "r". That is the font's kerning, not a layout fault.

Startup after the review fixes, builds alternating, 12 dry starts each at this
PC's 150 percent (`interleave.ps1` in the scratch folder; medians in ms). The
first two columns are one run, the last two another. This PC ran about 15 ms
slower than earlier in the day for every build.

| Measure                 | Before the fix | Fixed | Before HarfBuzz | Fixed |
| ----------------------- | -------------- | ----- | --------------- | ----- |
| Home shown              | 41.1           | 28.8  | 28.8            | 27.7  |
| First screen loaded     | 75.5           | 62.2  | 61.7            | 60.2  |
| First frame (gate: 250) | 228.4          | 229.1 | 225.9           | 223.2 |

To repeat the width check after changing `src/xerces_rmlui_harfbuzz` or updating
RmlUi or HarfBuzz: build the launcher with HarfBuzz, then
`tools\dev\launcher\font-width-check.ps1 -ToolsDir C:\Users\Ginld\XercesDev\tools`
(exit code 0 when every line fits; it builds the check into
`build\font-width-check`).

### Crash reports (2026-11-05)

`docs/CRASH_REPORTS.md` has the contract. Built and checked without the game:
the unit tests, `xerces_cli crash list` and `crash send` against a stand-in
backend on 127.0.0.1 (`XERCES_CRASH_URL`; a pending crash became a report, went
out gzipped with the install id and came back 201), and the launcher's Privacy
screens with `--dry-launch` (the switch, "Delete my crash reports", first run
step 04). Still to check in the game, with a build from this branch and the
backend deployed with the crash routes:

- Force a crash: add `-xerces-crash-test` in Settings > Extra launch flags,
  start the game from Xerces, open the console and run `xerces_crash_test` (an
  access violation), `xerces_crash_test fastfail` (0xC0000409, no dump) or
  `xerces_crash_test fatal` (a fatal engine error). Remove the flag afterwards.
- [ ] Setting on, Xerces open: the dialog's title is "Xerces", it names no Ezz
      site, and it says Xerces sends a crash report as soon as the game closes.
      After "Close game", the footer says "Crash report sent." with a server id,
      and `<home>\crash\pending` and `outbox` are empty.
- [ ] The report is in the backend: `GET /v1/admin/crashes` (admin token) lists
      it with kind `exception`, the module and frames, `has_minidump` true; the
      report has no `C:\Users\<your name>`, no player name and no server
      address. The minidump (`GET /v1/admin/crashes/{id}/minidump`) opens in
      WinDbg with thread stacks only; its module paths read
      `C:\Users\_____\...`, and a search of the file for your Windows user name
      and your in-game name, as ASCII and as UTF-16 (a hex editor), finds
      nothing.
- [ ] Setting on, Xerces closed (close it after the game starts): the dialog
      says the report is sent when Xerces opens next; opening Xerces sends it.
- [ ] `xerces_crash_test fastfail`: no dialog; the footer says the report was
      sent, kind `fail_fast` in the backend.
- [ ] `xerces_crash_test fatal`: the fatal-error dialog says Xerces sends a
      crash report. Press "Restart game" (the default, which ends the first game
      with exit code 0): the backend has a report of kind `unknown_exit` with
      frames and a game module, and the console tail holds the error. Quit the
      restarted game, then repeat with "Close game": a second report.
- [ ] Chat in a self-hosted match with a second PC (both players type), then
      `xerces_crash_test` on one of them: the report's console tail has no chat
      line, and neither player's in-game name appears anywhere in it (names the
      console printed show as `<player>`), also when the name in Settings
      differs from the one the game uses (Steam's).
- [ ] Setting off: the dialog says crash reports are off and points to
      `https://github.com/DeroXP/boiii-free/issues` and Diagnostics > Copy
      report; nothing appears in `<home>\crash\pending`, `outbox` or the
      backend.
- [ ] Out of memory (a custom map that overflows with DS4C, 5.6): kind
      `out_of_memory`, with the map and the console lines.
- [ ] Settings > Privacy > "Delete my crash reports": the footer says how many
      were deleted, and the admin list has none from this install.
- [ ] A crash in the game with Xerces' hooks on the stack no longer says
      boiii.exe "was loaded into the game by other software"; a crash inside
      boiii.exe itself says it is a bug in the Xerces game client.

### Joining another tester's game (two PCs, built 2026-11-05)

DESIGN.md 11.10; the steps for you and the friend with the beta zip are in
`docs/TWO_PC_TEST.md` (H hosts, J joins). Built, unit-tested
(`src/xerces_tests/test_join.cpp`) and checked with dry launches only; none of
it has run on two PCs.

- [ ] LAN, vanilla to vanilla: J joins H's open lobby, then H's running match,
      by address. With the lobby closed again, J gets the message that H is in
      the menus.
- [ ] Leaving the lobby for the main menu closes it to direct connect within
      about 15 s (whether BO3 keeps H's party lobby in the main menu is not
      known; if it does, the lobby stays open there).
- [ ] A joiner missing one stacked mod gets one dialog naming it with its
      Workshop page; Yes opens it in Steam. After subscribing, J's game loads
      H's mods and joins again by itself, once.
- [ ] A join that stops at that dialog leaves J's own mods: J's main menu still
      names them, not H's.
- [ ] Several missing items are named in one dialog ("Open their 2 Workshop
      pages"), and Yes opens each page in Steam: every page can be reached with
      Steam's Back button and subscribed to. If only the last one can, the
      dialog has to open one page only.
- [ ] The D3 stack on both: J joins H's lobby and match with no "Different
      versions" dialog and plays with all three mods.
- [ ] "Different versions" names the item whose files differ, and Yes joins
      anyway. Needs two versions of one item, for example a copy under `mods\`
      changed on one PC.
- [ ] A host on a custom map with map-first: J follows H's `mapScriptsFirst`,
      also with `xerces_map_scripts_first 0` on J, and plays.
- [ ] A custom map J lacks: joining H's match or lobby names it with its
      Workshop page; H switching to it drops J to the menu with "You don't have
      the map ..." and offers the page, with no reconnect.
- [ ] Over the internet: opening the lobby maps the UDP port on H's router (the
      "Router port open" toast shows the address), and J joins with it. Closing
      the lobby and quitting the game remove the mapping.
- [ ] The toasts for a router without UPnP, a router that refuses the port, a
      shared (carrier-grade NAT) outside address, and a forwarded port whose
      outside address the router did not report ("Router port open", no second
      router blamed).
- [ ] Quitting with the lobby open through the console window's close: the game
      ends within about 3 s more, also when the router does not answer.
- [ ] A host on another Xerces version gives J the "Xerces versions differ"
      toast; an older host or BOIII gives none, and joining works.
- [ ] A crash on H's game with other mods than J's plan: J's next start of
      Xerces does not offer safe mode for J's own mods (`joined` in J's
      journal).
- [ ] A culprit test launch that joins H's game with other mods records no
      result: Home says the last test did not count and shows the same test
      (`launcher.culprit.joined` in the launcher log).
- [ ] Copy report and the console text from both PCs after the test.

The network password checked by the host (DESIGN.md 11.10, "The network
password"; TWO_PC_TEST test 6). Built 2026-11-05 and unit-tested
(`src/xerces_tests/test_join_password.cpp`: HMAC-SHA256 and PBKDF2 against RFC
4231 and the published vectors, the host's salt, the messages, the host's
challenges and checks, a spoofed answer or second challenge that leaves the
joiner's challenge alone, the address rules, who gets in without the check, the
wrong-answer limits per address and in all with more addresses than the tables
hold, and the challenge limit); not run in the game or on two PCs.

- [ ] A join with `xerces_join_skip_password_check 1` and no password is refused
      by H: "This game needs a password, and the host's game refused the join".
- [ ] J without `net_password`: "This game needs a password." from J's own game.
- [ ] J with a wrong password: "Wrong password."; H's console says a joiner gave
      a wrong password. After five wrong tries within a minute, the sixth says
      "Wrong password too many times" with the seconds to wait.
- [ ] J with the right password joins H's lobby, follows H into the match, and
      joins the running match again after leaving.
- [ ] With `net_password` set, H's own game enters its own match: its own player
      comes through the engine's loopback (`netadrtype_t::LOOPBACK`), which
      skips the check, and H's console has no "[Join] Refused a connect". This
      is assumed, not yet seen.
- [ ] From J's PC, PowerShell's raw `lobbyJoin` gets `lobbyDenied ... password`,
      and a raw `connect` while H plays gets the `error` "This game needs a
      password, and the host's game refused the connect". The same from H's own
      PC with 127.0.0.1 (a relay or tunnel agent on the host's PC gets no free
      pass).
- [ ] J types `set net_password ""` after joining: J stays in H's lobby and
      follows H into the match.
- [ ] A `--with-ezz-social` build: a friend's join to a host with a password
      gets `lobbyDenied`, answers the check, and joins with the right password
      ("Wrong password." with a wrong one).
- [ ] Three PCs: a joiner's party member who comes along into H's lobby is
      refused at the match's connect, and gets in by joining H by address with
      the password (TWO_PC_TEST test 6, step 11).
- [ ] Three PCs: host migration with `net_password` still set on the members,
      and with it cleared after joining (TWO_PC_TEST test 6, step 12).
- [ ] H without a password: joins work as before.
- [ ] An older Xerces build or BOIII with any password stops at its own check
      ("Server requires a network password." or "Network password mismatch.").
- [ ] Over the internet with UPnP, the right password passes (the host's outside
      address is the one the router reported), and when the router did not
      report one (H gives the public address by hand).
- [ ] H's game log has `join.password.host` with `ok` and `wrong`, and J's
      `join.password.joiner`; neither has an address or the password.

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

### Precise frame limiter (2026-11-05)

`-xerces-precise-limiter` (DESIGN.md 12.4, Precise frame limiter). Settings,
Display, "Precise FPS limit (experimental)" adds it to the extra flags.

- [x] (2026-11-05, offline) `test_frame_pacing.cpp`: the engine's loop on a fake
      clock. At 240, frames leave 4.132 to 4.201 ms apart (upstream's wait: 3.01
      to 5.14 ms); 200, 250 and 1000 FPS stay within 0.05 ms of their period;
      `com_maxfps 0` and caps under 20 give exactly upstream's timeline; a
      15.625 ms system timer costs at most 1.3 ms of spin a frame. Added after
      the review: an event loop of up to 0.3 ms (3.88 to 4.46 ms, mean 4.167),
      one that grows with its queue (no change), a timer that cannot be set
      (spins, counted in `timer_errors`), and the release counts in the stats.
- [x] (2026-11-05, dry launches only, home
      `C:\Users\Ginld\XercesHome-agent-frame-limiter`) The toggle saves
      `-xerces-precise-limiter` in `launcher.json`, the dry launch logs
      `-launch -nointro -xerces-precise-limiter`, and turning it off removes it.
      Safe mode and culprit tests leave it out (unit test).
- [x] (2026-11-05, unit tests and dry launcher runs, home
      `C:\Users\Ginld\XercesHome-agent-frame-limiter-review`) Crash recovery:
      the journal records `precise_limiter`; after a crash with it the safe-mode
      message names it, and the culprit search first runs the whole crashed
      stack without it, so a crash it causes is not called "these mods together"
      (`test_m1_runtime`, `test_launcher_core`). On a hand-written journal, Home
      showed that message, then "Test 1: launch the crashed mods again without
      the precise FPS limit"; with both settings found, "Turn both off" removed
      both flags from `launcher.json`.
- [ ] In the game, with your go-ahead: an A/B at `com_maxfps 240`, windowed, on
      Cheese Cube, 3 runs a side in A-B-B-A order:

  ```
  tools\dev\bench\game-bench.ps1 -Variant w240 -Dvars @{com_maxfps='240'} -OutDir C:\Users\Ginld\XercesDev\bench\<session> -Live
  tools\dev\bench\game-bench.ps1 -Variant w240p -Dvars @{com_maxfps='240'} -ExtraFlags -xerces-precise-limiter -OutDir C:\Users\Ginld\XercesDev\bench\<session> -Live
  xerces_cli bench compare --a <session>\w240 --b <session>\w240p --source probe
  ```

  `bench compare` reads only `frames.csv`. For a run's `main.csv`, in
  PowerShell:

  ```
  $d = Import-Csv <run>.main.csv | Select-Object -Skip 5 | ForEach-Object { [double]$_.dt_ms } | Sort-Object
  $d[[int]($d.Count * 0.01)], $d[[int]($d.Count * 0.5)], $d[[int]($d.Count * 0.99)]
  ```

  - The game log has `xerces.frame_pacing.on`, then `xerces.frame_pacing.stats`
    every 30 s: `paced` close to `frames`, `released_under_100us` close to
    `paced` (the limiter let those frames go within 0.1 ms of their deadline),
    `released_p99_us` well under 1000, `tick_waits` a small share, `tick_missed`
    and `timer_errors` 0, `spin_us` about 0.2 to 0.3 ms per frame.
  - The probe's `main.csv` stamps each main frame at its end, after its work, so
    it shows the release spacing plus the change in work from frame to frame; a
    tight tolerance around 4.167 does not apply. Compare p1 to p99 between the
    two sides: on 2026-11-05 `w240` had p1 3.19 ms and p99 5.59 ms. From the
    uncapped runs' work, perfect releases would give about 3.5 to 4.8 ms, and a
    median near 4.167.
  - `frames.csv`: mean 239 to 240 FPS; p99 and the 1% low better than w240's
    (6.05 ms and 158.1 FPS on 2026-11-05). The render thread may keep its 0.5 ms
    steps (DESIGN.md 12.4); note how much.
  - Aiming feels the same or smoother, with no added input lag.

- [ ] In the same session: `com_maxfps` 60, 144 and 0 from the console take
      effect at the next frame. At 0 the stats line still comes every 30 s, with
      `frames` 0 and `maxfps` 0 (`upstream_waits` only in very fast menus). At
      10 it is the same as without the flag: about 17 FPS, `frames` 0. Dragging
      the in-game Max FPS slider stays smooth; alt-tab and minimize do not hang
      the game; quitting is normal.

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

## Zombies character screen (beta.18, beta.19)

Built on `launcher/upscale`; the test home is
`C:\Users\Ginld\XercesDev\bench\20261106-modlabels` (DS4C Patched on top of Hyperdimension
Neptunia, `zm_zod`), driven with `bench\20261108-chars\run-lua.ps1` (keys,
screenshots, console, Lua snippets).

- [x] The pause menu has CHARACTERS (in DS4C's own pause list, above RESTART LEVEL).
- [x] The screen lists DS4C's characters below index 64 with their portraits and a
      scroll bar; "Looks from Hyperdimension Neptunia [Character Mod]" on Dempsey to
      Takeo; no Lua error.
- [x] A character with outfits (Floyd) opens an outfit screen (Floyd 1 to 5).
- [x] Picking Richtofen closes both menus and puts Neptunia's Richtofen on the
      player (third person and first-person arms); with a test script that called
      `SetCharacterBodyType` as DS4C does, the calls were skipped (`char.call` lines
      "(skipped: locked)") and the player kept the pick.
- [x] After a pick the view goes third person (Neptunia's character seen from behind) and
      back to first person; the screen itself cannot show the model (see DESIGN.md 7.10).
- [x] The host's pick is put on again after `map zm_zod` (outfit 3 of Floyd came back as
      type 5, style 2).
- [ ] Not tried: another player picking, DEFAULT after a pick, GFL or Avali stacked, a
      dedicated server.

## Zombies modern aim assist and no hip-fire spread (beta.20)

Built on `launcher/upscale`; test home `C:\Users\Ginld\XercesDev\bench\20261106-modlabels`,
`zm_zod` with no mods, driven with `bench\20261108-chars\run-lua.ps1`.

Aim assist (`XERCES_AIM_TEST=1`: a simulated controller with the left stick held
sideways; no real controller was used):

- [x] `xerces_aim_debug`: camera ok (eye 60 units over the player's origin, tangents
      1.167 / 0.656 at a field of view of 100), client angles ok.
- [x] A zombie at 28 to 32 m walking through the bubble: rotation 0.09 at the bubble's
      edge (e 0.96), 0.30 (e 0.88), 0.49 (e 0.80), 0.60 inside three quarters (e 0.74);
      slowdown 0.30 from the hip; over a second the target moved 0.33 degrees and the view
      followed 0.20 (60%); the client's yaw and the camera's both went from 0.00 to 1.04.
- [x] "Modern, Stronger": rotation 0.29 at 49 m rising to 0.80 from 35 m in, slowdown
      0.36 from the hip; the view followed 80% and kept a zombie in the middle of the
      bubble (e about 0.1) as it walked from 48 m to 26 m.
- [x] No crash or freeze over four runs (five matches, two with the assist on) with the
      final build, leaving to the menu included. Two freezes on the way (a game call and
      a log line during a level load) are fixed and noted in DESIGN.md 7.11.
- [ ] Not tried: a real controller (how it feels, the 2-second idle rule, the right
      stick's slowdown, the sights-up rule), a strafe past a still target at 10 m (19
      degrees in Black Ops 6), co-op, a joined match.

No hip-fire spread:

- [x] Ten shots from the hip at a wall: a spread of holes about a third of the screen's
      height wide with the setting off, one tight group at the crosshair with it on
      (`bench\20261108-chars\spread-compare.png`).
- [x] Log: "No hip-fire spread on 94 weapons" as the match starts; after `disconnect` and
      a second match with the setting off, "Hip-fire spread back on 64 weapons (94 were
      changed)" as that level starts; clean exit.
- [x] XERCES SETTINGS' lists build with the new rows (ZOMBIES 11 rows, CONTROLLER 1, seven
      tab entries) and no Lua or script error; the menu itself was not opened on screen.
- [ ] Not tried: co-op (a second player's shots), turning the setting off in the middle
      of a match, a custom map's own weapons loaded late.

## Zombies: every character mod's characters as slots, stronger aim assist (beta.21)

Built on `launcher/upscale`; test home `C:\Users\Ginld\XercesDev\bench\20261106-modlabels`,
`zm_zod`, driven with `bench\20261108-chars\run-lua.ps1`. The stack is the user's own:
DS4C Patched, Ascendance, im_oxo Zombies QOL, Classic Viewmodel FOV, DBZ Villain
Characters, Girls' Frontline 2.0, Avali: Solspace, Hyperdimension Neptunia.

Character slots:

- [x] The log: "Character slots: 73 characters from 6 sources beside the table in use
      (69 outfits added, 0 left out for room)", 1.8 to 3.0 ms (the first version, with
      `VirtualQuery`: 9.7 s).
- [x] Slots put on with the menu response and seen in third person: Neptunia's Noire
      (0:10), Girls' Frontline's M16A1 (0:2) and a character on outfit 14 (9:14), Avali's
      Thomas Frost, the game's own Dempsey (0:13) while DBZ's Cooler is the Dempsey in use
      (`bench\20261108-chars\looks2-*.png`).
- [x] CHARACTERS on screen: DEFAULT, ASCENDANCE (1), DBZ VILLAIN CHARACTERS, GIRLS'
      FRONTLINE, AVALI: SOLSPACE, HYPERDIMENSION NEPTU.., ORIGINAL CHARACTERS, then DS4C's
      roster; each name on one row (`menu3-menu.png`).
- [x] DBZ's screen: Cooler, Broly, Cell, Frieza and their second places, "In Nikolai's
      place: the game still names and voices you as Nikolai. Model: dv_brolyfz_fb"
      (`menu5-dbzrow.png`). Girls' Frontline's: M16A1, AK-12, HK416... by its table's names.
- [x] A pick in Neptunia's screen: all three menus close, Neptune in third person, her
      own hands in first person (`menu4-neptune.png`, `menu4-nephands.png`). A pick of an
      outfit of DS4C's roster closes all menus too (it left the first open before).
- [x] Saved pick: `xerces_char_saved` 38 and `xerces_char_saved_look` 2005565732 after
      picking Neptune; `disconnect`, a second match: the slots are worked out again (the
      log line again), both dvars as before, and `SetCharacterBodyType 2`,
      `SetCharacterBodyStyle 5` nine seconds after the spawn (`XERCES_CHAR_TRACE=1`).
- [x] DS4C's own calls on a locked player are still skipped; no Lua or script error; no
      crash or freeze in fourteen matches, leaving to the menu and stopping the game
      included.
- [x] Without DS4C (Girls' Frontline on top, Neptunia under it): 26 characters from 2
      sources, 25 outfits added; Neptunia's Noire (slot 0:4) seen in third person while
      Girls' Frontline's table is the one in use (`gfl1-pick.png`). With no mods: nothing
      added, nothing logged.
- [ ] Not tried: two players and a joined match, a custom map with its own character
      table, a Chronicles map (the `_zmhd` table), a mod reload from the mods menu between
      matches.

Aim assist (`XERCES_AIM_TEST=1`, no mods, no real controller):

- [x] "Modern (MW3)": rotation 0.75 and slowdown 0.51 from the hip on zombies from 4 m to
      26 m; over a second the target moved 2.65 degrees and the view followed 1.99 (75%);
      at 0.8 m rotation 0.53 and slowdown 0.46; at the bubble's edge (e 0.99) 0.03.
- [x] "Modern, Stronger": rotation 0.95, slowdown 0.64.
- [x] 250 to 265 frames a second with it on, no crash; the log's "Aim assist is off"
      line as the level ended is now said only when it lasts.
- [ ] Not tried: a real controller.

## CHARACTERS as one screen with a live view, the client gone over, text reports (beta.22)

Built on `launcher/upscale` on 2026-11-12; test home
`C:\Users\Ginld\XercesDev\bench\20261106-modlabels`, `zm_zod`, the user's eight-mod stack
(as in beta.21's section). The game was driven with keys and console commands
(`bench\20261108-chars\run-lua.ps1`, `key:` and `shot:` steps); `lua_exec` crashed the
UI's Lua VM twice that night and is not used for tests any more.

CHARACTERS (DESIGN.md 7.13; runs `ui11` and, after the fixes below, `fix1` and `fix3`):

- [x] Pause menu > CHARACTERS closes the pause menu and opens the screen over the match:
      tabs DS4C PATCHE.., ASCENDANCE, DBZ VILLAIN.., GIRLS' FRON.., AVALI, HYPERDIMENS..,
      ORIGINAL CH..; the list on a dark panel at the left, the player's character from the
      front at the right (`bench\20261108-chars\fix1-grid.png`).
- [x] Q and E change tab. Moving down a list changes the character shown (DS4C's, DBZ's,
      Neptunia's, Ascendance's Misty seen).
- [x] Back puts the look from before back. A pick keeps it: the camera stays for a moment,
      the screen closes, the hands in first person are the character's.
- [x] Opened again, the pick is the selected row. DEFAULT puts the match's own look back. A
      second match in the same game works the same.
- [x] The log: "Character slots: 103 characters from 7 sources, the table in use among
      them" (beta.21 counted 73 beside the table in use; the screen now lists that table's
      own 30 too).
- [x] No Lua or script error in the console of either run; the round HUD and the pause
      menu's Xerces rows are there after the fixes.
- [x] A second stack, Girls' Frontline 2.0 on top with Neptunia under it and no DS4C
      (`bench\20261112-crash\session.ps1`, keys sent one step at a time): the first try
      showed the street and no character. The camera had not followed the angles it was
      given (DESIGN.md 7.13). Aimed at an entity instead, the character stands right of the
      list: Girls' Frontline's default and M16A1 3, Neptunia's Uni; Back returns the view
      (`bench\20261112-crash\exp2-grid.png`). The log there: "66 characters from 3
      sources".
- [x] The DS4C stack again with the camera aimed that way (`fix3`): the default look, Broly
      in DBZ's tab and Ram in Neptunia's, each from the front and right of the list; a pick
      of Ram keeps her and her hands; opened again, the view shows her; Back returns
      (`bench\20261108-chars\fix3-grid.png`); no Lua or script error.
- [x] One mod that adds no characters (Classic Viewmodel FOV): one list without tabs,
      "Characters: 13 characters", the game's own Dempsey 2 and Nikolai 3 shown live, a
      pick kept (`bench\20261112-crash\base-grid.png`).
- [x] A player who is down gets the list without the view (they lay under the camera's
      sight in two test matches where the zombies had reached them first).
- [x] A second, independent read of the screen's scripts found two defects that no solo
      test had met (DESIGN.md 7.13, the last three points). After the fixes: with
      `PREVIEW_TIMEOUT` set to 1, the camera, the HUD and the first-person view come back
      by themselves 5 s after the screen opened, the screen still open
      (`bench\20261112-crash\to-grid.png`); with no saved pick, the match's own character
      (red hair, black beret), a pick of M16A1 3 through the preview ("M16A1 3 (PICKED)"),
      then DEFAULT: the match's own character again (`nd-crops.png`). With the saved pick
      the user's config has (178), DEFAULT gives the match's character, not the pick.
- [x] Kino der Toten (`zm_theater`, Zombies Chronicles, the `_zmhd` table) with the
      eight-mod stack: "114 characters from 7 sources", then the match ends 13 s in with
      "ClientSysSetState - state index (-1) out of bounds (0 - 8)" from the map's
      `zm_theater_magic_box.gsc` when the power comes on (the test home has Power On at
      Start). With no mods and the same Xerces settings the match runs; so it does with
      DS4C alone and with Ascendance, im_oxo QOL, DBZ, Girls' Frontline, Avali or
      Classic Viewmodel FOV alone; the seven mods without DS4C fail as the eight do. It
      takes several of the mods together (KNOWN_ISSUES.md). With the eight mods and
      `xerces_zm_powerOn 0` the match was still running 40 s in, no script error. The
      Giant (`zm_factory`) is not installed on this PC.
- [x] The final build once more through `m1-check.ps1 -Live`: 60 checks pass, none fails.
- [x] The client systems on Kino, read from the game (a test thread that listed
      `level._clientSys` into a dvar, since removed): no mods "n=4 box_indicator=3 lsm=2
      levelNotify=1 musicCmd=0"; the eight mods "n=9 box_indicator=-1 lsm=7
      ds4c_pts_color=6 ds4c_print=5 zm_character_icon=4 ds4c_lua_text=3 levelNotify=2
      clientsystem=1 musicCmd=0". With `drop_failed_client_systems`: the eight mods and
      Power On at Start on, still in the match 30 s after it began, no script error.
- [x] Two players, the second a bot (the console's `spawnBot 1`, "[EXE]Aimbot.exe
      Connected"; a line added for the test, since removed, wrote what the preview's safety
      saw into a dvar): "players 2 safe 0 held -1", so nothing is held and the player can
      be hurt, as meant; the preview shows the default look and Dempsey 2
      (`bench\20261112-crash\bot-grid.png`); the player, standing still, is knocked down
      about 45 s into the match and the preview ends by itself (its safety stops being
      asked); both players go down, the match ends in the After Action Report, the game
      quits cleanly; no script or Lua error.
- [ ] Not tried: a second person's own game (what they see of the previews, their own
      screen), a controller's LB/RB (Q and E were pressed), the fall-back without the HUD
      menu, a custom map, CHARACTERS on a Chronicles map (the match above ended first).

The fixes of DESIGN.md 7.14:

- [x] `fix1`, the eight-mod stack: no "LUI event name is not precached" in 4,400 console
      lines; CHARACTERS still offered (the host has Xerces' scripts); the round HUD's three
      lines; the pause menu's rows.
- [x] The aim assist after its fixes (`fix2`, `XERCES_AIM_TEST=1`, the eight-mod stack,
      "Modern (MW3)"): no target and rotation 0 before the first zombie; after the pause
      menu had been open and closed, rotation 0.75 and slowdown 0.51 on a zombie at 25 m,
      0.71 and 0.51 on one at 37 m, as in beta.21; no jump of the view on the first frame
      back.
- [x] XERCES SETTINGS in the Zombies lobby (the entry in a match opens the match's own
      list, which has no tabs): the strip shows ZOMBIES, GAMBLING, ROUNDS, GRAPHICS and
      CONTROLLER; E four times reaches CONTROLLER (Aim Assist: Game's Own), Q goes back to
      GRAPHICS (`bench\20261112-crash\lobby-grid3.png`).
- [x] Launcher, core, CLI (14 fixes): RmlUi opens its files as UTF-8 (a Xerces folder with
      a character outside the ANSI code page loaded no font or page); a Steam library on
      such a path is found, and the same Steam root is listed once; a pin an update left
      behind offers "Unpin"; the friends listener stops cleanly; the saved sign-in stays
      out of config backups; the CLI reads the profile folder and user name as UTF-8; a run
      file caught half written no longer makes a running game look crashed (and is written
      through a rename); Up and Down reach every control of Settings (a test now compares
      the list with `settings.rml`); fastfiles at a Workshop item's root are not counted
      as loose files in conflicts; a beta build meets a mod's `requires.xerces` as its
      release does; the asset pool switch leaves a `launcher_settings.json` it cannot read
      alone; a file held open for a moment (antivirus) is still replaced, and two programs
      writing the same file no longer share one temporary name; the CLI says when it could
      not write a file; nothing after an invite link on Xerces' command line is read as an
      option.
- [x] `xerces_tests`: 599 pass (585 before; 13 for the launcher fixes, 1 for the crash
      report's new parts).
- [x] `tools\dev\launcher\check-screens.ps1`: all ten screens captured, 0 warnings or
      errors (`launcher-checks\beta22`).
- [x] `tools\dev\session\m1-check.ps1 -Live`, all nine cases through Xerces.exe (vanilla 4
      checks, modded 6, cli with load and unload 9, reopen 8, cliapply 8, dvars 8, crash 7,
      restart 4, safeoff 6): 60 pass, none fails.
- [x] The other pause-menu entries on the eight-mod stack after the round script's fixes:
      PACK-A-PUNCH WHEEL answers "Hold a Pack-a-Punched gun to spin the wheel", COIN FLIP
      opens its bets (`bench\20261112-crash\sweep2-grid.png`).
- [x] The package itself (`C:\Games\xerces-0.1.0-beta.22`, started with
      `session.ps1 -Bin`): a `zm_zod` match on the eight-mod stack, CHARACTERS with its
      tabs and the live view, a pick; the log's "103 characters from 7 sources". Windows
      Firewall asked about the new `boiii.exe` (a new folder is a new program to it); the
      question was left for the user to answer.
- [ ] Not seen on screen: the Mods menu's delete, FOV Scale's row, a starting round at the
      game's cap, a folder outside the ANSI code page (the unit tests cover it).

Text reports (CRASH_REPORTS.md, "The text report"; `bench\20261112-crash\run-crash.ps1`
starts the game with `-xerces-crash-test -quiet-crash` and turns crash reports on in the
test home for the run; nothing is sent):

- [x] `xerces_crash_test` in a `zm_zod` match with the eight-mod stack: exit 0xC0000005;
      `crash-<time>.txt` (11 KB) with the sentence ("invalid memory access: tried to write
      to 0x0 (null pointer) in boiii.exe+0x1E3F2B ... on the main thread"), the session
      (map, the eight mods with their titles, 1 min 27 s), the stack, the bytes at the
      address, memory (5.8 of 31.7 GB of RAM free, the game at 15.8 GB), the zone pools and
      the settings; the same text as `info.txt` in the zip; the note for the launcher has
      reason `exception`, the summary and the text (4.3 KB).
- [x] `xerces_crash_test freeze` in the same match: `freeze-<time>.txt` at 30 s, 439 lines:
      the main thread in full (its sleep, called from the test command), the server thread
      in full, one running thread, then the waiting ones ("10 threads at the same place");
      the game closed itself 60 s after its last frame with 0xC0000194; the note has reason
      `freeze`, the summary and the dump (611 KB).
- [x] `xerces_crash_test stall` in the menus: the report at 30 s; at 40 s the console says
      "[Freeze] Frames came back after 40 seconds: it was a long stall, not a freeze", the
      note for the launcher is gone, the game plays on and quits cleanly.
- [x] `xerces_crash_test fatal` in the menus: exit 1; `fatal-<time>.txt` with the error's
      words and where it was raised, `fatal-<time>.dmp` (615 KB); the note has reason
      `fatal_error`, the error as its summary and "[Fatal error] ..." as the console's
      last line.
- [x] A report written off the main thread had no settings (the game answers for its dvars
      on the main thread only); the main thread notes them every 5 s now and the stall's
      report has them.
- [x] End to end against the backend run on this PC (`uvicorn`, SQLite,
      `XERCES_CRASH_URL=http://127.0.0.1:8765`, `xerces_cli crash send` on a scratch home
      holding the three notes): three reports stored; `scripts\crashes.py list` prints each
      with its summary, `show` prints the reason, the summary and the game's report before
      the logs; the Windows user name is nowhere in what `show` prints (the source paths
      read `%USERPROFILE%\...`).
- [x] The backend's tests: 455 pass. Its change is commit `baec036e` on
      `launcher/friends-backend`, not pushed: Railway runs the backend from before, which
      drops the four new parts and keeps the rest of the report.
- [x] The crash dialog without `-quiet-crash`: "Crash report (crash-2026-11-12-06-57-18.txt)
      and dump saved in: ...\boiii\minidumps", then the line about sending (its text was
      read through accessibility: Windows Firewall's question lay over it). It comes up
      13 s after the crash, when the zip is written.
- [x] A second, independent read of the new report code found four defects and several
      things to harden (CRASH_REPORTS.md, "The text report"): the watchdog logged, and
      asked for the journal's phase, before it ended a frozen game; every dump stopped
      the watchdog; a `__try` block does not catch a fault of its own instructions with
      this compiler (the stack walk's read, and all of `safe_memory.hpp`); a stall's
      report kept its place among the game's three. All fixed, then tested:
- [x] `xerces_crash_test heap` (the main thread holds the heap's lock for good): nothing
      can be written, and the game closes itself after 62 s with 0xC0000194.
- [x] `xerces_crash_test guard`: "xerces_safe on a bad address: read 0, write 0, text 0
      (all 0: the faults were caught)", and the game plays on.
- [x] With those changes, again: `freeze` (closed at 60 s, text 29 KB, dump 602 KB, the
      note and its dump for the launcher; the report does not say the dump stopped the
      watchdog), `stall` (report at 30 s, taken back at 40 s, the game plays on), a crash
      (0xC0000005; dump 954 KB, text, zip 21 MB, note) and `fatal` (exit 1; text, dump
      622 KB, note). The eight-mod stack once more (`fix4`): 103 characters from 7
      sources, the live view, a pick, and the aim assist's rotation 0.56 at 42 m rising
      to 0.75 at 35 m.
- [x] The backend as Railway runs it today (`8affa1fc`, started on this PC from a scratch
      checkout): the final build's three reports (crash, freeze, fatal error) are taken
      and stored, the four new parts dropped as unknown; `show` has the freeze's summary
      as the console tail's last line. The deployed service itself answers 405 to
      `GET /v1/crashes`, which means it takes reports.
- [x] A freeze with the server's thread stuck inside a script, as on the night this was
      written for (`xerces_crash_test script`, then a character picked in CHARACTERS: the
      script call that sets the character never returns). The report: "The main thread
      is waiting in a system call (NtWaitForSingleObject+0x14), called from
      BlackOps3.exe+0x14212BCF3. The server thread is waiting in a system call
      (ZwDelayExecution+0x14), called from boiii.exe+0x1DF444
      (...SetCharacterBodyType+0x2B). Script running: GSC xerces_characters::wear
      (custom_scripts/zm/xerces_characters.gsc line 1099)", then the script stack (`wear`,
      called from `menu_responses`) and the server thread's stack down into the game's
      script VM; the game closed itself with 0xC0000194. The line numbers of Xerces' own
      scripts count the lines of the headers they insert (about 960 too many here); the
      function names are right.
- [ ] Not tried: a crash on the server thread, a report sent to Railway itself.

One thing found on the way: the scrubbing of logs and reports read `name.ext:12` as a host
and a port and wrote `<address>`. The game's own log had "lua/shared/<address>: function
expected instead of nil" for a Lua error, and a report's source lines went the same way.
A name that ends as a file does (lua, gsc, csc, gsh, cpp, hpp, c, h, inl, json, cfg, txt,
log) is left as it is now (`xerces_core/log.cpp`, `file_ending`; `xerces_tests` checks
both that and that `lua.example.org:27017` is still scrubbed). The text report writes a
script's line as `scripts/zm/zm_zod.gsc line 12` all the same.
