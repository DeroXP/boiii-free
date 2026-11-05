# Testing Xerces

What runs by itself, and the checklist for what only the real game on your PC
can show (docs/DESIGN.md 15.3 and 15.4). The checklist grows every milestone.

## Automated

| What                                                                                                                                                             | Command                                                        | Needs                         |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- | ----------------------------- |
| Core unit tests: manifests, SemVer, the planner and its determinism, merges, conflict rules, hook chains, packs and share codes, logs, the loose both-apply pair | `tools\build.ps1 -Tests`                                       | LLVM 23, Ninja, premake 5.0.0 |
| CLI smoke: plan, conflicts and pack round trip on the fixtures                                                                                                   | `tools\build.ps1 -Tests`                                       | the same                      |
| CLI smoke on a home: mods enable, disable, order and info, `plan apply --dry-launch`, `packs apply`, `logs tail`, `sig check` on a fake game folder              | `tools\build.ps1 -Tests` (`tools\ci\cli_smoke.ps1`)            | the same                      |
| Signature registry against a stored dump (`compat/signatures.json`)                                                                                              | `xerces_cli sig check --dump C:\Users\Ginld\XercesDev\dumps`   | a dump on this PC             |
| Pre-flight check of the installed game against `compat/builds.json`                                                                                              | `xerces_cli game check`                                        | the game on this PC           |
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

  Open: the loaded-mod label ("â€¦ and 2 more") was not checked, and each mod's
  feature in play needs a person playing.

- [ ] Joining a server whose mods you lack names the missing mods instead of
      offering a download (the Xerces change in `check_valid_mod_stack`). Not
      run: it needs a server.
- [x] Two fastfiles that share a name: all three mods ship `core_mod.ff`. With
      mod-stack's aliases PMem raised no error in three cases: the load, a front
      end â†’ match â†’ front end round trip, and a main-mod switch that
      unloaded every stacked zone in reverse order.
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
      run: PresentMon is not installed, and downloading it is yours to approve.
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

### Loader and manifest (M1)

What the game role does since 389dff95 (`src/client/xerces`): it resolves
`compat/signatures.json` against the running game before mod_stack hooks
anything (multi-mod stays off on a miss), reports it in
`<home>\cache\signatures-report.json`, checks `fs_game` and `fs_extramods`
against the plan file named by `-xerces-plan`, and moves the journal
(`<home>\logs\journal.json`) through `symbols`, `frontend`, `in_game` and
`clean_exit`. Its log is `<home>\logs\<date>-<time>-game-<pid>.jsonl`. None of
it has run in the game yet: these items need you (and your go-ahead to start the
game).

`tools\dev\session\m1-check.ps1 -Case vanilla|modded|crash|restart|cli -Live`
runs the first five items one case per run through `Xerces.exe` and prints pass
or fail per check (journal phases, game-log events, `fs_game` and `fs_extramods`
from the console, the launcher's view of the exit, a capture of Home after the
crash). Without `-Live` it only does a dry launch; that form passed on
2026-11-05 and builds the D3 command line. Its home is
`C:\Users\Ginld\XercesHome-m1`.

- [ ] Launch vanilla from `Xerces.exe` reaches the menu. The game log shows
      `xerces.signatures.resolved` (9 of 9) and the journal reads `frontend`;
      quitting from the menu leaves `clean_exit`.
- [ ] Launch modded with the D3 stack (1944930083, then 3171632315 and
      3656779213): `xerces.feature.on` and `loader.plan.applied` in the game
      log, and the three mods load as they did in D3.
- [ ] A forced crash (end `boiii.exe` in Task Manager during a match) leads to
      safe mode on the next launcher start; "Launch with mods off" reaches the
      menu without mods.
- [ ] After that crash, "Restart game" in the crash dialog: the restarted game
      has no `XERCES_LAUNCH_ID`, so it leaves the journal alone and the launcher
      still offers safe mode for the launch that crashed.
- [ ] Close `Xerces.exe` while the game runs and open it again: Home does not
      call the running game a crash (it finds the game's entry in `<home>\run`).
- [ ] Settings changed in the launcher win over the game's saved config: the
      game log shows `xerces.dvars.reapplied` after the config loads.
- [ ] Find the culprit: one test launch per step, each result recorded when the
      game exits (checked with written journals only, see Launcher below).
- [ ] With the game in the menus: `xerces_cli status` shows the build, the plan
      and the stack (it finds the game's pipe in `<home>\run`);
      `xerces_cli load --extras 3171632315,3656779213` and `xerces_cli unload`
      change the extra mods and reload them, as the mods menu does; in a match
      they answer that the stack only changes in the menus. The pipe itself is
      tested in `test_ipc.cpp` (a served pipe, a forged run file refused).
- [ ] `xerces_cli plan apply` (no `--dry-launch`; it starts the game, so only
      with your go-ahead), from a build with `boiii.exe` next to
      `xerces_cli.exe` (`tools\build.ps1 -Client`), with the D3 stack as the
      Zombies profile and `XERCES_HOME` outside AppData: the game starts as it
      does from Launch modded, takes the journal over, logs
      `loader.plan.applied`, and the three mods load. This is M1's "a plan with
      two Workshop mods loads through the CLI". Afterwards
      `xerces_cli sig check` reads the report the game wrote (9 of 9, exit 0)
      and `xerces_cli logs tail --role game` shows its log.
- [x] (2026-11-05, dry launches only) `xerces_cli plan apply --dry-launch` on a
      copy of the D3 profile (`C:\Users\Ginld\XercesHome-cli`) prints
      `-launch -nointro -xerces-plan <id> +set fs_game 1944930083 +set fs_extramods 3171632315,3656779213`
      with the plan id `Xerces.exe`'s dry launch logged on the same home, writes
      the plan file and no journal.

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
      longer wins over Xerces' (launch_dvars.cpp).
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

### Conflict system (M2)

- [ ] `xerces_cli test run both-apply` with the loose pair: the merged table
      shows both rows and both edits, and `xerces_test_hooks` reads
      `fixture.alpha;fixture.beta;probe;`.
- [ ] The same with the fastfile pair built with the BO3 Mod Tools.
- [ ] The pre-launch conflict report matches what happens in the game for two
      legacy mods that replace the same script.
