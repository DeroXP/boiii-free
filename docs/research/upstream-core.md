# Upstream core architecture

> Machine-generated research notes from 2026-11-04, kept as evidence for
> `docs/DESIGN.md`. Code was read at upstream `main` 5aa7fac, `mod-stack`
> ac356c3c and the fork `refs/heads/main` 7805319. `path:line` citations refer
> to those commits. Treat claims as leads: anything marked runtime still needs
> the real game.

## Summary

Upstream runs the game inside its own process. After parsing flags, boiii.exe
resolves and enters the game folder, runs the updater, the WebView2 launcher and
the exe-compatibility check, and then constructs all components. It maps
BlackOps3.exe (or BlackOps3_UnrankedDedicatedServer.exe) with LoadLibraryA,
snaps imports by hand, and sets up the game's TLS through a small helper DLL
extracted to %TEMP%. It then points PEB.ImageBaseAddress at the game, so
GetModuleHandle(NULL) returns BlackOps3.exe. Next it patches a few IAT entries
(steam_api64, ExitProcess, SetProcessDPIAware), runs post_load and jumps to the
game's entry point. The game's protected startup eventually calls
SetProcessDPIAware or SteamAPI_RestartAppIfNecessary. Both are IAT-hooked, and
that call runs post_unpack, where 72 of the 77 components install their
game-code hooks. ExitProcess triggers pre_destroy. There is no post_start phase:
constructors act as a pre-load phase. post_load users only touch OS APIs and
IATs. That post_load runs before Arxan unpacks the code and post_unpack after is
inferred from the call order, the phase names and the -dump help text, not
observed.

Components register through static initializers. They are ordered by a small
priority enum (arxan highest, then updater, steam_proxy, name). Everything else
shares priority 'min', so its order is the link order of translation units, not
anything stated in source. Game addresses are compile-time constants. There are
3,548 symbol<> objects, each with up to three absolute addresses: the Sept-2026
client (CheckSum 0x6531394, 3,433 non-zero), the legacy March-2023 client
(0x888C368, 3,080) and the dedicated server (0x14C28B4, 2,030). The column is
picked at runtime from the mapped image's PE CheckSum, then rebased. On top of
that come 138 inline game::select sites, 62 '_g' literals, about 585 server-only
relocate() table entries (564 in dedicated/), 49 symbol.offset() mid-function
patches and 26 legacy-only crash-fix RVAs. A generic pattern scanner exists
(utils::hook::signature, '_sig'), but its only users are in the fenced
arxan.cpp. It scans the whole 471 MiB image linearly. All 3,200 named code
symbols of the installed build sit in the first .text (47 MiB), so a
section-scoped, cached signature registry seeded from the existing three columns
fits well. It would resolve at the start of post_unpack. The registry's
validation would also catch three malformed decimal literals found in the
tables.

The installed exe is exactly upstream's pinned build. It has CheckSum 0x6531394,
and its SHA-1 9D03F810...3B9B matches the constant in main.cpp. The legacy
0x888C368 build still runs (it is the build the DeroXP fork's PoC addresses
target). Any other checksum triggers a prompt to download the pinned exe from
archive.org: the hash is verified, the file is swapped, and the backup is then
deleted. -newsteamclient was the opt-in flag for the new client from April to
September 2026 and was removed in 4b4fd331. boiii.exe can already live outside
the game folder, via game_path.txt, Steam detection or a folder picker. However,
an inverted check in find_steam_game_path (main.cpp:649) makes non-default
installs loop forever when launched from outside the folder. User data is spread
across %LOCALAPPDATA%\boiii, %LOCALAPPDATA%\EZZ BOIII\WebView2,
<game>\boiii_players, <game>\boiii, <game>\logs and %TEMP%. The updater deletes
non-manifest content under %LOCALAPPDATA%\boiii (including plugins/ and
minidumps/) and data/. In Release it would replace a Xerces boiii.exe with
upstream's binary.

Release builds have almost no file logging: the trace log is debug-only. What
remains is the console or stdout, crash zips, plugins.log, a Lua error log and
the server's games_mp.log. -headless only reroutes console and error output. The
client still shows the launcher (unless -launch), the splash and the game
window. Only a dedicated server run with -headless is windowless, and the server
exe is not installed on this PC. Plugins are any DLLs in two folders, loaded
before the game is mapped, with optional exports p_name, post_load, post_unpack
and pre_destroy and no host API or ABI version. A dev/RE module can ship this
way today, but a missing p_name export crashes the loader. Xerces needs a
versioned plugin ABI.

The rename touches about 230 branded lines in 57 source files, plus the
launcher, Lua UI, README, docs and CI. Brand strings (titles, URLs, user agent,
Discord app ID, executable and folder names) must be kept apart from
compatibility identifiers. Those include Engine.IsBOIII, the rich-presence keys,
the folders holding user data and the XUID key salts in the fenced auth.cpp.
Ownership and anti-tamper code is wired into the core path. main.cpp's
patch_imports installs the Steam emulation (`src/client/steam/**`). Further
fenced pieces are steam_proxy's ownership and app-480 logic, auth.cpp, the
DemonWare auth emulation, arxan.cpp and integrity.hpp, symbol::call_safe, and
the TAC (Treyarch anti-cheat) circumvention code. These are listed in
off_limits. The lifecycle has a non-Steam post_unpack trigger
(SetProcessDPIAware, verified as imported by the installed exe) that the Xerces
core can depend on instead.

## Facts

- **verified**: boiii.exe runs the game inside its own process. It maps
  BlackOps3.exe (or BlackOps3_UnrankedDedicatedServer.exe) with LoadLibraryA,
  snaps imports manually, wires the game's TLS, sets PEB->ImageBaseAddress to
  the game image, and calls the game's entry point on its own main thread.
  (src/client/main.cpp:535-543, 1075, 1106-1107;
  src/client/loader/loader.cpp:15-72, 134-184; src/common/utils/nt.cpp:9)
- **likely**: Relocations are applied manually only under Wine. On Windows the
  OS mapping of the LoadLibrary'd EXE applies ASLR relocation, while its imports
  are not snapped by the OS, which is why load_imports always runs.
  (loader.cpp:74-76 (is_wine guard), 179-181)
- **verified**: The game's TLS lives in a separate no-CRT DLL (0x2000-byte
  thread_local payload) embedded as RCDATA 307 and extracted to
  %TEMP%\boiii-tlsdll.dll. Only one allocation is supported. The game's TLS
  callbacks are invoked by boiii's thread_local tls_runner for threads created
  after the entry handoff. (src/tlsdll/dllmain.cpp:5-8;
  src/client/resource.r<local scratch copy, not kept>104-108;
  src/client/loader/tls.cpp:12-40; src/common/utils/binary_resource.cpp:9-41;
  main.cpp:506-533, 1106)
- **verified**: boiii.exe is linked at image base 0x170000000, directly above
  the 0x140000000+0x30000000 range that ptr.hpp treats as engine space. Every
  game address is rebased at runtime via relocate(), so the game may load at any
  base (installed exe has DYNAMIC_BASE, DllCharacteristics 0x8160).
  (premake5.lua:489-494; src/client/game/ptr.hpp:32-42; PE header of installed
  BlackOps3.exe)
- **verified**: Lifecycle phases are: (1) constructor, run in
  component_loader::activate before the game is mapped; (2) post_load, after
  mapping, identity check and IAT patching, before the game entry runs; (3)
  post_unpack, run once from inside the game's call to SetProcessDPIAware or
  SteamAPI_RestartAppIfNecessary; (4) pre_destroy, run once from the ExitProcess
  hook, the early-exit finally or the static deleter. There is no post_start.
  (component_interface.hpp:26-43; component_loader.cpp:53-198; main.cpp:32-35,
  52-75, 1008-1013, 1071, 1093)
- **likely**: post_load runs before the protected game code is unpacked and
  post_unpack after. Every non-fenced post_load user only hooks OS APIs or IATs
  (LoadIconA, the WS2_32 IAT, RtlSetUnhandledExceptionFilter, MiniDumpWriteDump)
  or starts threads. (main.cpp:1093-1107 call order;
  src/common/utils/flags.cpp:169-173 (-dump help text 'after Arxan unpack');
  icon.cpp:33-35; demonware.cpp:352-363, 391-405; exception.cpp:876-895;
  scheduler.cpp:261-268)
- **verified**: The installed client imports every function boiii hooks through
  the IAT: USER32!SetProcessDPIAware, KERNEL32!ExitProcess, 16 steam_api64
  functions including SteamAPI_RestartAppIfNecessary, plus
  d3d11!D3D11CreateDevice, dxgi!CreateDXGIFactory1 and 35 WS2_32 functions.
  (Import table of <local scratch copy, not kept>\Program Files
  (x86)\Steam\steamapps\common\Call of Duty Black Ops III\BlackOps3.exe parsed
  read-only)
- **verified**: Components are ordered by the priority enum min < name <
  steam_proxy < updater < log (debug only) < arxan, stable-sorted in descending
  order. All other components share 'min', so their relative order is
  static-initializer (link) order rather than anything declared in source.
  (component_interface.hpp:5-18; component_loader.cpp:32-42;
  component_loader.hpp:26-40)
- **verified**: 77 components are registered: 49 generic, 19 client-only and 9
  server-only. 72 implement post_unpack, 10 post_load and 17 pre_destroy. The
  log component and name() exist only in debug builds. (REGISTER_COMPONENT scan
  of `src/client/component/**`; component/log.cpp:2, 22-23;
  component_interface.hpp:29-31)
- **verified**: An error in activate or post_load makes main return 1. An
  exception in post_unpack or pre_destroy shows an error and calls
  TerminateProcess(1). Components are not isolated from each other.
  (component_loader.cpp:53-74, 76-117, 139-158, 182-197; main.cpp:1071-1095)
- **verified**: Game addresses are compile-time constants: WEAK (inline
  constexpr) symbol<T>{new_client, legacy_client, server}. get() picks a column
  by PE CheckSum (server 0x14C28B4, new client 0x6531394, otherwise legacy
  0x888C368) and rebases it from 0x140000000.
  (src/client/game/symbols/macros.hpp:4-6; symbol.hpp:12-57; ptr.hpp:35-84;
  base.cpp:30-54)
- **verified**: There are 3,548 symbol<> declarations: 3,482 in game/symbols, 60
  in game/structs and 6 in component/currency.cpp. They hold 3,433 non-zero
  new-client, 3,080 legacy-client and 2,030 server addresses (115 server-only).
  lua/cod/api.hpp alone has 1,719. (Parser over all symbol<...>{...}
  declarations (scratchpad count_symbols.py))
- **verified**: Outside the symbol tables there are: 138 inline
  game::select(...) sites; 62 '0x..._g' literals; about 585 server-only
  relocate() table entries (21 in dedicated/core.cpp, 543 in
  dedicated/vehicle.cpp, about 21 elsewhere); 49 symbol.offset(0x..)
  mid-function patch sites (33 in auth.cpp); and 26 legacy-only crash-fix RVAs.
  In total src/client contains 10,694 address literals with a 0x14 or 0x15
  prefix, 1,223 of them in the fenced integrity.hpp. (grep counts over
  src/client; exception.cpp:595-741; dedicated/core.cpp:27-32, 65-69;
  dedicated/vehicle.cpp:20 onward)
- **verified**: Of the new-client symbol addresses, 3,200 fall in the first
  .text section (RVA 0x1000, 47.0 MiB), 227 in .data, 3 in .rdata and 3 in
  .interpr. None fall in the second executable section (RVA 0x1AA6B000, 44.9
  MiB). (Section table of the installed BlackOps3.exe combined with the parsed
  symbol addresses (scratchpad bucket_symbols.py))
- **verified**: Three symbol literals are decimal numbers missing the 0x prefix
  and resolve to invalid pointers: the Con_DrawInput server column 140182010,
  the I_strcat server column 140581110 and the db_hashTable legacy
  column 146904070. (src/client/game/symbols/console.hpp:10-11; quake.hpp:17-18;
  db/core.hpp:10)
- **verified**: A pattern scanner already exists (utils::hook::signature and the
  _sig literal). Patterns are hex with one '?' per wildcard byte. By default it
  scans the whole SizeOfImage of GetModuleHandle(NULL) with a multi-threaded
  linear search. Its only users are six scans in the fenced arxan.cpp.
  (src/common/utils/signature.hpp:6-46; signature.cpp:39-261;
  src/client/component/arxan.cpp:636-696)
- **likely**: The scanner's SSE4.2 path is effectively dead because
  has_sse_support() is compiled only when `__SSE4_1__` and `__SSE4_2__` are
  defined, while the build passes -mno-sse4.1 -mno-sse4.2.
  (signature.cpp:239-255; premake5.lua:322-328, 371-377)
- **verified**: Dedicated-server addresses are effectively frozen: the server
  CheckSum 0x14C28B4 dates from 2023-01-01 and the server build time string is
  'Mon Dec 16 10:44:20 2019'. (git log -S0x14C28B4 (2617fbd8, 2023-01-01);
  src/client/game/symbols/com/core.hpp:18-19)
- **verified**: The installed BlackOps3.exe (106,059,256 bytes, TimeDateStamp
  2026-08-11T18:00:53Z, CheckSum 0x6531394, SHA-1
  9D03F81086112113BFB1DD22B8538B9398AC3B9B) is byte-identical to upstream's
  pinned 'supported' build, so the patch installer does nothing on this PC.
  BlackOps3_UnrankedDedicatedServer.exe is not installed. (PE header read and
  sha1sum, both read-only; main.cpp:124-132; directory listing of the game
  folder)
- **verified**: Pre-load compatibility check: for a legacy or unknown CheckSum
  the client prompts (except under -headless) and downloads the pinned exe from
  archive.org (black-ops-3_20260926, fallback _20260927). It checks the SHA-1
  and CheckSum, kills any running BlackOps3.exe if the user allowed it, swaps
  the files via a backup, then deletes the backup. Cancelling continues with the
  current exe. (main.cpp:124-132, 141-211, 304-313, 334-504, 1068)
- **verified**: Post-load identity check: after mapping, main throws
  'Incompatible game version detected' unless the mapped CheckSum is the
  server's (for dedicated) or the legacy or new client's. The legacy 0x888C368
  build still runs. (main.cpp:1080-1089, 272-282; base.cpp:36-54)
- **verified**: -newsteamclient was an opt-in flag (b678a08d, 2026-04-17) that
  selected a newer client checksum, first 0x6517980. e1e73075 (2026-09-10)
  switched it to 0x6531394 and stopped the auto-downgrade of the Sept-2026 exe
  when the flag was set. 4b4fd331 (2026-09-25) removed the flag, made 0x6531394
  the default and 0x888C368 legacy, and dropped 0x8880704. (git show b678a08d,
  481217da, 47ead16f, e1e73075, 4b4fd331 (diffs of src/client/game/base.cpp and
  src/client/main.cpp))
- **verified**: The DeroXP fork targets the build upstream now calls legacy: in
  the fork is_client() checks 0x888C368, is_legacy_client() checks 0x8880704,
  and symbols have two columns (client, server).
  (refs/heads/main:src/client/game/game.cpp:31-47;
  refs/heads/main:src/client/game/game.hpp:41-58)
- **verified**: The game's build/version string is spoofed to the Sept-2026
  client on every build ('Wed Dec 10 17:10:46 2025', CL 0xD3FC12).
  (src/client/game/impl/com/g_com.cpp:39-46, 82-85; symbols/com/core.hpp:8-20)
- **verified**: data/launcher/verification.json is a report-only game-file
  manifest (path, size, XXH3-64 hash, component) used by the launcher's Verify
  feature. Its BlackOps3.exe entry (106,059,256 bytes) matches the installed
  build. (data/launcher/verification.json:1-41; launcher.cpp:206-236, 253-600)
- **verified**: %LOCALAPPDATA%\boiii is computed by two independent functions.
  The game folder is the cwd, cached on first use. game_directory() returns the
  folder of the mapped BlackOps3.exe. (game.cpp:30-51; properties.cpp:109-120;
  path.cpp:13-16; base.cpp:56-58)
- **likely**: boiii.exe can already live outside the game folder. Before
  anything else, the cwd is set to the game folder found by: a cwd check,
  %LOCALAPPDATA%\boiii\user\game_path.txt, Steam detection, or a folder picker.
  All user paths are relative to the cwd. (main.cpp:635-893, 1019-1036)
- **likely**: find_steam_game_path has an inverted check: it returns the default
  Steam path when that path is NOT a valid game folder. resolve_game_path then
  reports success with an invalid folder, and main's retry loop spins forever
  for non-default installs launched from outside the game folder.
  (main.cpp:649-651, 723-731, 1030-1036)
- **verified**: The CRT file APIs (_fsopen, _wfsopen, _mkdir, _stat64,
  _stat64i32) and FS_BuildOSPath are detoured through path::normalize. It
  redirects players to boiii_players, \LPC to <game>\LPC, and workshop content
  to usermaps/mods when no Steam workshop folder exists. (patches.cpp:437-524,
  576; path.cpp:97-149)
- **verified**: The updater fetches https://r2.ezz.lol/boiii.json (or
  boiii-beta.json). It deletes every %LOCALAPPDATA%\boiii root entry missing
  from the manifest except user/ and data/, and deletes non-manifest files under
  data/. In Release builds it replaces the running boiii.exe, wherever it lives,
  when its SHA-1 differs and selectedVersion is 'latest'.
  (file_updater.cpp:17-25, 44-47, 190-201, 498-512, 537-543, 637-713;
  io.hpp:34-36)
- **verified**: For clients the updater runs twice: synchronously in main
  (unless BlackOps3.exe or boiii.exe is already running), and again on a thread
  started in the updater component's constructor, which is joined in
  post_unpack. (main.cpp:1045-1047; src/client/component/updater.cpp:40-84)
- **verified**: Release builds write no loader or component log. game::trace is
  a no-op unless NDEBUG is undefined. In debug builds a Logger thread appends
  '[<ctime>.<ms>][Debug] msg' lines to <exe dir>\debug.log (debug-server.log on
  servers) or to the path given by -tracing. (src/client/game/log.hpp:16-105;
  src/client/game/log.cpp:12-46; src/client/game/flags.cpp:36-71)
- **verified**: Logs produced in Release: the console window or stdout; crash
  zips at %LOCALAPPDATA%\boiii\minidumps\ezz-crash-<timestamp>.zip (crash.dmp +
  info.txt); <game>\boiii_players\plugins.log;
  <exe dir>\logs\boiii_lua_errors.log; and the server's games_mp.log.
  (exception.cpp:366-374, 473-573; plugins.cpp:22-27, 85-100;
  ui_scripting.cpp:1702-1721; game_log.cpp:50-91)
- **verified**: -headless redirects console text and errors to stdout and skips
  the boiii console window, the exe-patch prompt and the updater windows. It
  does not skip the WebView2 launcher (that needs -launch), the splash window,
  the crash dialog and Explorer window (needs -quiet-crash), or the client's
  game window. Only a dedicated server run with -headless is windowless.
  (console.cpp:1318-1320, 1580-1597, 1646-1647; game.cpp:22-23; main.cpp:306,
  1063-1066; splash.cpp:143-146; exception.cpp:285-291; updater_ui.cpp:12-13)
- **verified**: Dedicated mode (-dedicated, or only the server exe present)
  skips the synchronous updater, the launcher UI and exe patching, and activates
  the 9 server-only components plus the generic ones. dedicated/networking.cpp
  adds connection rate limits, chat sanitising and an RCE patch.
  (main.cpp:1038-1069; `src/client/component/dedicated/*.cpp`;
  dedicated/networking.cpp:23-330)
- **verified**: Plugins: every `*.dll` in %LOCALAPPDATA%\boiii\plugins and
  `<game>\boiii\plugins` is loaded with LoadLibrary in the plugins component's
  constructor, before the game is mapped. The optional exports are p_name,
  post_load, post_unpack and pre_destroy, none taking arguments. There is no
  host API and no ABI version. (plugins.cpp:18-74, 102-142)
- **likely**: A plugin without a p_name export, or a DLL that fails to load,
  reaches std::string(nullptr) at plugins.cpp:128, because library::invoke
  returns T{} instead of throwing. -noplugins is ignored on dedicated servers.
  (plugins.cpp:29, 116-128; src/common/utils/nt.hpp:88-150)
- **verified**: Lua mods can load native DLLs through package.loadlib once
  unsafe Lua is approved (by dialog or -unsafe-lua). -disable-loadlib turns this
  off. (ui_scripting.cpp:56-86, 1285-1348, 1769;
  src/common/utils/flags.cpp:73-77, 229-234)
- **verified**: Several flags do nothing or less than advertised: -plugins,
  -trimlogs, -keep-launcher and -port are declared but never read, and -safe
  (help text: 'disable mods') only turns off fullscreen and vsync.
  (src/common/utils/flags.cpp:102-146; startup_flags.cpp:19-22; grep for
  has_flag uses)
- **verified**: The scheduler has six pipelines: async (thread started in
  post_load), renderer (SND_EndFrame detour, client only), server
  (G_ClearVehicleInputs detour), main (Com_Frame_Try_Block_Function detour),
  dvars_flags_patched and dvars_loaded. scheduler::on_game_initialized is
  declared but never defined. (scheduler.hpp:6-40; scheduler.cpp:74-86, 143-159,
  261-279; dvar_patches.cpp:98; dvars.cpp:166)
- **verified**: The 26 crash-fix patches in the vectored exception handler apply
  only to the legacy client, not to the installed 0x6531394 build.
  (exception.cpp:578-741 (is_legacy_client() guard at line 595))
- **verified**: The launcher's version picker downloads boiii.exe assets from
  the Ezz-lol/boiii-free GitHub releases and the r2.ezz.lol beta into
  <game>\versions and runs them. -beta does the same for servers from
  r2.ezz.lol. (data/launcher/main.js:4463-4530; launcher.cpp:1315-1355;
  main.cpp:924-990)
- **verified**: An ImGui overlay with an IDXGISwapChain::Present hook (vtable
  index 8, taken from a dummy device) already exists.
  (src/client/component/download_overlay.cpp:351-446)
- **verified**: security.hpp is lobby-packet validation ported from T7Patch, not
  anti-tamper code. (src/client/component/security.hpp:9-25, 101-108, 176-180)
- **verified**: Upstream describes itself as removing Steam ownership
  verification ('No Steam ownership required'). The project is licensed GPL-3.0.
  (README.md:37-49; LICENSE:1-2)
- **verified**: Branding scale: src has 151 lines containing 'boiii' and 76
  containing 'ezz' across 57 files; data has 132 occurrences in 17 files; README
  43 lines; docs 17 lines; .github 38 occurrences; build scripts and
  package.json 8 lines. (grep -rIi counts, with false positives in keys.txt, .gz
  resources and hash_names.txt excluded)

## Details

# Upstream core architecture, Xerces working tree (branch `xerces` = Ezz-lol/boiii-free main @5aa7fac)

Every path:line below is in the working tree unless prefixed `refs/heads/main:`
(the DeroXP fork). "Verified" means I read it in the code, or measured it
read-only on this PC. Anything inferred is labelled as inferred.

## (a) Process and loader model

### Startup call flow (client unless noted)

```
boiii.exe WinMain (main.cpp:1110) -> main (main.cpp:992)
 0  static init: each REGISTER_COMPONENT installer pushes a factory          component_loader.hpp:26-40
 1  handle_process_runner "-proc <pid>": wait on parent, exit                 main.cpp:545-562  [fenced]
 2  utils::flags::parse_flags (argparse parse_known_args; skips +set etc.)    main.cpp:1001; utils/flags.cpp:6-12, 51-249
 3  enable_dpi_awareness; validate_non_network_share; remove_crash_file       main.cpp:1005-1017
 4  resolve_game_path -> SetCurrentDirectoryA(game dir)                       main.cpp:694-893
 5  is_server = -dedicated || (no BlackOps3.exe && server exe present)       main.cpp:1023-1039
 6  server && -beta: download r2.ezz.lol/boiii/beta/boiii.exe -> <game>\versions\boiii-beta.exe, spawn, exit   main.cpp:924-990
 7  client && no BlackOps3.exe/boiii.exe running: updater::update (sync)      main.cpp:1045-1047
 8  abort if %LOCALAPPDATA%\boiii\data\launcher\main.html missing            main.cpp:895-898, 1049-1058
 9  client: GPU-pref registry value; launcher::run() unless -launch (may relaunch self with "-launch"); ensure_compatible_client_binary   main.cpp:1060-1069
10  component_loader::activate(is_server): construct components               main.cpp:1071
11  load_process -> loader::load_binary                                         main.cpp:1075, 535-543
      LoadLibraryA("BlackOps3.exe" | "BlackOps3_UnrankedDedicatedServer.exe")  nt.cpp:9
      load_relocations  (only when running under Wine)                         loader.cpp:74-132
      load_imports      (manual IAT snapping: LoadLibraryA + GetProcAddress)    loader.cpp:15-72
      load_tls          (game TLS -> slot of boiii-tlsdll.dll)                  loader.cpp:134-170; tls.cpp:15-40
      PEB->ImageBaseAddress (Reserved3[1], +0x10) = game base                   main.cpp:538-540
12  identity check on the mapped image's PE CheckSum                           main.cpp:1080-1089; base.cpp:30-54
13  patch_imports: steam_api64 IAT -> src/client/steam [fenced];
      ExitProcess -> exit_hook; SetProcessDPIAware -> set_process_dpi_aware_stub   main.cpp:77-111
14  component_loader::post_load()                                               main.cpp:1093
15  g_call_tls_callbacks = true; entry_point()  (does not return normally)     main.cpp:1106-1107
16  the game calls SetProcessDPIAware or SteamAPI_RestartAppIfNecessary -> component_loader::post_unpack() (once)   main.cpp:52-75
17  the game calls ExitProcess -> exit_hook -> component_loader::pre_destroy() -> ExitProcess   main.cpp:32-35
```

- The game runs inside the boiii.exe process, and the OS process name is
  boiii.exe. launcher.cpp:1423-1458 detects "BlackOps3.exe" and other
  "boiii.exe" processes.
- An EXE loaded through LoadLibrary is mapped and ASLR-relocated by the OS, but
  its imports are not snapped and its entry point is not called. That is why
  load_imports always runs while load_relocations runs only under Wine
  (loader.cpp:75). This Windows behaviour is inferred from how the code is
  structured.
- After step 11, GetModuleHandle(NULL), `utils::nt::library{}` and
  `game::get_base()` (base.hpp:21-23) all refer to the game.
  `game::get_engine_base()` throws "Invalid host application" if called before
  the swap (base.cpp:9-21).
- boiii.exe is linked at `/base:0x170000000` (premake5.lua:489-494). That is
  directly above the 0x140000000 + 0x30000000 range that ptr.hpp:32-33 treats as
  engine space. The installed exe has SizeOfImage 0x1D75BC00, 11 sections, entry
  RVA 0x2BD334C and DllCharacteristics 0x8160 (DYNAMIC_BASE set). Every address
  therefore goes through relocate().
- TLS:
  - src/tlsdll is a no-CRT DLL exposing an 0x2000-byte thread_local block
    (tlsdll/dllmain.cpp:5-8). It is embedded as RCDATA TLS_DLL=307
    (resource.r<local scratch copy, not kept>104-108), extracted to
    %TEMP%\boiii-tlsdll.dll (tls.cpp:12; binary_resource.cpp:9-41) and loaded.
  - load_tls (loader.cpp:134-170) then:
    - points the game's AddressOfIndex at the DLL's TLS index;
    - copies the game's TLS template into the current thread and into the DLL's
      template;
    - nulls the DLL's callbacks;
    - overwrites the DLL's TLS directory with the game's.
  - Only one allocation is supported (tls.cpp:16-22).
  - boiii's thread_local `tls_runner` calls the game's own TLS callbacks on
    DLL_THREAD_ATTACH/DETACH, but only for threads created after the entry
    handoff (main.cpp:506-533).
  - The installed exe has a TLS directory at RVA 0x30E2850 and a 0x400-byte .tls
    section (read from the PE header).
- Imports of the installed client (import table parsed read-only):
  - USER32!SetProcessDPIAware
  - KERNEL32!ExitProcess
  - steam_api64: 16 imports, including SteamAPI_RestartAppIfNecessary,
    SteamAPI_Init and SteamInternal_CreateInterface
  - d3d11!D3D11CreateDevice
  - dxgi!CreateDXGIFactory1
  - WS2_32: 35 imports
  - MiniDumpWriteDump is not statically imported; exception.cpp detours it after
    loading dbghelp itself.

### Component lifecycle

| Phase                          | Trigger                                                                                                                                                     | Game image                                                                                             | Used for                                                                                                                                                                                                                                                                                      | Failure                                                             |
| ------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| static registration            | boiii CRT init                                                                                                                                              | not mapped                                                                                             | REGISTER_COMPONENT pushes a factory tagged any, client or server. Registering after activation throws "Registration is too late" (component_loader.cpp:45-51)                                                                                                                                 | -                                                                   |
| constructor (acts as pre-load) | activate(is_server), main.cpp:1071; instantiates `any` plus the matching type (component_loader.cpp:53-74)                                                  | not mapped                                                                                             | exception filter, headless console, splash thread, updater thread, plugin LoadLibrary. Components with constructors: console, demonware, exception, plugins, splash, updater                                                                                                                  | show_error, then main returns 1                                     |
| post_load                      | main.cpp:1093, after mapping, identity check and patch_imports, before the entry runs                                                                       | mapped; protected code not yet unpacked (inferred)                                                     | OS- and IAT-level hooks only: icon LoadIconA; demonware WS2_32 IAT (demonware.cpp:352-363, 391-405); exception RtlSetUnhandledExceptionFilter and MiniDumpWriteDump (exception.cpp:876-895); scheduler async thread; discord; name; log thread; plugins; steam_proxy [fenced]; arxan [fenced] | false, then main returns 1                                          |
| post_unpack                    | first of the SetProcessDPIAware / SteamAPI_RestartAppIfNecessary IAT stubs (main.cpp:52-75), on the game's thread; runs once (static guard)                 | unpacked (inferred from the phase name, call order and the -dump help text at utils/flags.cpp:169-173) | 72 of 77 components; almost every game hook                                                                                                                                                                                                                                                   | show_error, then TerminateProcess(1) (component_loader.cpp:139-158) |
| pre_destroy                    | ExitProcess hook (main.cpp:32-35), main's premature_shutdown finally (main.cpp:1008-1013), component vector deleter (component_loader.cpp:17-21); runs once | -                                                                                                      | thread joins, IAT restore                                                                                                                                                                                                                                                                     | TerminateProcess(1)                                                 |

- There is no post_start phase (component_interface.hpp:26-43). The fork's
  interface is identical
  (refs/heads/main:src/client/loader/component_interface.hpp).
- Ordering:
  - The priority enum is min < name < steam_proxy < updater < log (debug only) <
    arxan (component_interface.hpp:5-18).
  - Components are stable-sorted in descending priority after every insert
    (component_loader.cpp:32-42).
  - About 70 components share `min`. Their relative order is static-initializer
    order, which is the link order of translation units.
- Inventory:
  - 77 registrations: 49 generic_component, 19 client_component, 9
    server_component.
  - 72 implement post_unpack, 10 post_load, 17 pre_destroy.
  - The log component exists only in debug builds (component/log.cpp:2, 22-23),
    and so does `name()`.
- Server components: console_command, game_log, rcon, dedicated/{command, core,
  info, networking, sound, vehicle}.
- Client components: binds, branding, client_patches, colors, currency,
  download_overlay, fov, friends, intro, live, live_presence_patch,
  localized_strings, nat, party, server_list, splash, startup_flags,
  steam_proxy, upnp.
- Scheduler pipelines (scheduler.hpp:6-26; scheduler.cpp:74-86, 143-159,
  261-279):
  - asyn<local scratch copy, not kept>wn thread, 10 ms period, started in
    post_load.
  - renderer: detour on SND_EndFrame, client only.
  - server: detour on G_ClearVehicleInputs.
  - main: detour on Com_Frame_Try_Block_Function. Servers wrap it in SEH plus
    setjmp restart logic.
  - dvars_flags_patched and dvars_loaded: executed by dvar_patches.cpp:98 and
    dvars.cpp:166.
- `scheduler::on_game_initialized` is declared (scheduler.hpp:39) but never
  defined. `component_loader::trigger_premature_shutdown` is never used.

## (b) Symbols and offsets

```cpp
// game/symbols/macros.hpp:4-6
#define WEAK inline constexpr
// game/symbols/db/core.hpp:11
WEAK symbol<void()> DB_Init{0x141422050, 0x141422030, 0x1401D7160};
//                         new client     legacy client  dedicated server
// ptr.hpp:62-84  select(): is_server() ? server : is_new_client() ? client : legacy  -> relocate(addr)
// ptr.hpp:35-42  relocate(): base + (addr - 0x140000000);  ptr.hpp:237  0x..._g == relocate
```

- base_symbol stores address, legacy_address and server_address
  (symbol.hpp:12-57). A zero means "absent in that build".
- Function symbols also expose call_safe() through the Arxan callstack proxy
  (symbol.hpp:69-73) [fenced].
- Because the objects are constexpr they cannot hold runtime-resolved state.

| Kind                                                   | Count                                                                                                                                                   |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| symbol<> declarations                                  | 3,548: 3,482 in `game/symbols/**`, 60 in `game/structs/**` (vehicle.hpp 23, phys/core.hpp 18, lua/hks.hpp 16), 6 in component/currency.cpp              |
| Largest files                                          | lua/cod/api.hpp 1,719; lua/cod/ui/element.hpp 248; snd/core.hpp 170; game/core.hpp 167; lua/hks.hpp 137; dvar/core.hpp 93                               |
| Non-zero new-client (0x6531394)                        | 3,433                                                                                                                                                   |
| Non-zero legacy client (0x888C368)                     | 3,080                                                                                                                                                   |
| Non-zero dedicated server (0x14C28B4)                  | 2,030 (115 server-only)                                                                                                                                 |
| Inline game::select(new, legacy, server) in components | 138 (security.hpp 15, network 13, loot 13, demonware 13, client_patches 10, dvar_patches 8, console 8, ...)                                             |
| `0x..._g` literals                                     | 62 (auth.cpp 22, dedicated/core 13, chat 6, game_log 5, ...), mostly single-build                                                                       |
| relocate() literal tables                              | dedicated/core.cpp 21; dedicated/vehicle.cpp 543 (server only)                                                                                          |
| symbol.offset(0x..) mid-function sites                 | 49 (auth.cpp 33, client_patches 10, network 4, workshop 1, branding 1)                                                                                  |
| Base-relative crash-fix RVAs                           | 26, applied only when is_legacy_client() (exception.cpp:595-741)                                                                                        |
| Raw 0x14xxxxxxx/0x15xxxxxxx literals in src/client     | 10,694 = game/symbols 8,368 + game/structs+impl 174 + components 2,152 (integrity.hpp 1,223 [fenced], dedicated/ 564, arxan.cpp 3 [fenced], others 362) |
| Hook install sites                                     | 190 detour `.create(`; 173 raw patch calls (call/jump/set/nop/copy, etc.) in components                                                                 |

Where the new-client addresses fall, using the installed exe's section map:

| Section                   | RVA        | Size     | Symbols                                                                                       |
| ------------------------- | ---------- | -------- | --------------------------------------------------------------------------------------------- |
| first .text               | 0x1000     | 47.0 MiB | 3,200 (1,169 function-typed, 2,031 function pointers typed as data, e.g. the Lua C API table) |
| .data                     | -          | -        | 227                                                                                           |
| .rdata                    | -          | -        | 3                                                                                             |
| .interpr                  | -          | -        | 3                                                                                             |
| second executable section | 0x1AA6B000 | 44.9 MiB | 0                                                                                             |

Executable sections total 92 MiB. SizeOfImage is about 471 MiB, most of it a 374
MiB .data section that is largely BSS.

Malformed entries (decimal literal without `0x`, giving a wild pointer after
relocate):

- `symbols/console.hpp:10-11`: Con_DrawInput server column `140182010`
- `symbols/quake.hpp:17-18`: I_strcat server column `140581110`
- `symbols/db/core.hpp:10`: db_hashTable legacy column `146904070` (this symbol
  has only two columns)

Server churn is near zero. The CheckSum 0x14C28B4 dates from commit 2617fbd8
(2023-01-01), and BUILD_TIME_SV is "Mon Dec 16 10:44:20 2019"
(com/core.hpp:18-19). All address churn is on the client.

### Existing scanning and RE utilities

- `utils::hook::signature` and the `"..."_sig` literal (signature.hpp:6-46;
  signature.cpp:39-261).
  - Pattern syntax: hex bytes, spaces ignored, one `?` per wildcard byte
    (signature.cpp:46-73). IDA-style `??` therefore yields two wildcard bytes.
    Trailing wildcards are trimmed.
  - Default range: the whole SizeOfImage of GetModuleHandle(NULL), i.e. the game
    after the PEB swap, about 471 MiB including .data.
  - Execution: serial for tiny ranges, otherwise hardware_concurrency()/2
    threads, with results sorted (signature.cpp:186-237).
  - The SSE4.2 path is reachable only when compiled with `__SSE4_1__` and
    `__SSE4_2__` (signature.cpp:239-255). Release passes
    `-mno-sse4.1 -mno-sse4.2` (premake5.lua:322-328), so in practice it is a
    linear byte loop. This is inferred from the flags.
  - Only users: 6 scans at arxan.cpp:636-696 [fenced].
- Other helpers:
  - `hook::extract<T>` and `follow_branch` for rel32 decoding
    (hook.hpp:232-238).
  - `hook::iat` (hook.hpp:160-163).
  - MinHook detours (hook.cpp:134-260).
  - asmjit assembler (hook.hpp:52-86).
  - udis86 disassembler (used at exception.cpp:750-772).

### How a signature registry fits

1. Codegen: parse `game/symbols/**` plus the inline select, `_g`, relocate and
   offset sites into one table. Each row holds id, name, kind (function, data or
   patch site), the RVA for each known build, and optionally a signature, an
   offset or an xref rule. The three existing columns become the "known build"
   rows, keyed by an exe identity of CheckSum + TimeDateStamp + SizeOfImage +
   SHA-1.
2. symbol<T> carries only an id (still constexpr). get() reads `resolved[id]`,
   already relocated, which is cheaper than today's select() + relocate() on
   every access.
3. Resolve once at the start of post_unpack, after the fenced arxan component
   (highest priority) has run:
   - Known build: take addresses from the table.
   - Unknown build: scan executable sections only (the first .text holds every
     named code symbol), using batched multi-pattern matching. Resolve data
     symbols by extracting RIP-relative references from code signatures.
   - Persist the results in a Xerces-owned cache keyed by the exe identity.
4. Validate at load: every address must lie in the expected section for its
   build, and every signature must match uniquely and agree with the known
   address. This check would have caught the three malformed literals.
5. post_load must stay limited to IAT- and OS-level work, as today.

## (c) Exe identity and version compatibility (behaviour only)

| CheckSum  | Upstream meaning now                                                                                             | History                                                                                                       |
| --------- | ---------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| 0x6531394 | new client, "supported": the Steam build released 2026-09-10 (the "September 2026 update")                       | added in 47ead16f (2026-09-10); default since 4b4fd331 (2026-09-25)                                           |
| 0x888C368 | legacy client, still runnable: is_client() = legacy or new (base.cpp:51-54), and the tables keep a legacy column | default "supported" until 4b4fd331; the DeroXP fork's target (refs/heads/main:src/client/game/game.cpp:37-41) |
| 0x6517980 | not recognised any more                                                                                          | -newsteamclient target in b678a08d (2026-04-17) and 481217da (2026-06-14) until e1e73075                      |
| 0x8880704 | dropped                                                                                                          | legacy before 4b4fd331; the fork's is_legacy_client (refs/heads/main:src/client/game/game.cpp:43-47)          |
| 0x14C28B4 | dedicated server                                                                                                 | unchanged since 2617fbd8 (2023-01-01)                                                                         |

Installed exe, measured read-only:

| Property      | Value                                                                                                   |
| ------------- | ------------------------------------------------------------------------------------------------------- |
| Size          | 106,059,256 bytes                                                                                       |
| TimeDateStamp | 0x6A7B6355 (2026-08-11T18:00:53Z)                                                                       |
| CheckSum      | 0x6531394                                                                                               |
| SHA-1         | 9D03F81086112113BFB1DD22B8538B9398AC3B9B, identical to `supported_client_patch_sha1` (main.cpp:131-132) |

So classify returns "supported" and nothing is downloaded on this PC.
BlackOps3_UnrankedDedicatedServer.exe is absent.

### Before load (client only, main.cpp:1068)

1. get_pe_checksum reads OptionalHeader.CheckSum from the file on disk
   (main.cpp:141-187).
2. classify_client_binary returns supported, legacy, incompatible or unreadable
   (main.cpp:195-211).
3. For legacy or incompatible, a MessageBox prompt appears; it is suppressed
   under -headless (main.cpp:304-313). Cancelling continues the launch with the
   current exe.
4. If the user accepts, install_supported_client_binary (main.cpp:334-480):
   - downloads to BlackOps3.exe.boiii_download from archive.org item
     black-ops-3_20260926, falling back to black-ops-3_20260927
     (main.cpp:127-130);
   - requires both the SHA-1 and the CheckSum to match (main.cpp:418-425);
   - terminates running BlackOps3.exe processes only if the user agreed
     (main.cpp:427-437, 242-270);
   - moves the original to .boiii_backup, moves the new file in and
     re-classifies it;
   - deletes the backup (main.cpp:439-475), so the user's original exe is not
     kept.

### After load

base.cpp:30-54 reads the CheckSum of the mapped image. main.cpp:1080-1089 throws
"Incompatible game version detected" (text at main.cpp:272-282) if the binary
kind does not match.

### -newsteamclient history

- b678a08d gated is_client() on the flag, and 481217da gated the expected
  checksum on it.
- e1e73075 (2026-09-10), "Don't auto-downgrade September 2026 update EXE if
  -newsteamclient flag is used". Before this commit the installer replaced a
  Sept-2026 exe with archive.org's t7_full_game (0x888C368) exe.
- 4b4fd331 removed the flag and flipped the pin to 0x6531394.
- The launcher's Settings > Patches tab still links
  archive.org/download/t7_full_game/BlackOps3.exe (main.html:2765-2786), which
  is now stale.

### Other version-related behaviour

- The version string seen on the network is spoofed to the Sept-2026 client
  ("Wed Dec 10 17:10:46 2025", CL 0xD3FC12) on every build
  (impl/com/g_com.cpp:39-46, 82-85; symbols/com/core.hpp:8-20).
- verification.json (1,599 lines) lists {path, size, XXH3-64, component} for
  each game file. The launcher's Verify button uses it and only reports
  (launcher.cpp:206-236, 253-600). Its BlackOps3.exe entry (line 41) is
  106,059,256 bytes, matching the installed exe.

## (d) Paths

| Root                                     | How determined                                                                                                                      | Code                                                                                                             |
| ---------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| game dir                                 | cwd after resolve_game_path, cached on first call                                                                                   | game.cpp:48-51; path.cpp:13-16                                                                                   |
| mapped exe dir                           | folder of the BlackOps3.exe module                                                                                                  | base.cpp:56-58; nt.hpp:51-53                                                                                     |
| %LOCALAPPDATA%\boiii                     | SHGetKnownFolderPath + "boiii", in two separate implementations; 36 call sites                                                      | game.cpp:30-46; properties.cpp:109-120                                                                           |
| ...\user\game_path.txt                   | remembered game dir                                                                                                                 | main.cpp:635-637                                                                                                 |
| ...\data\                                | launcher UI, verification.json, GSC/CSC script trees, lookup tables                                                                 | launcher.cpp:294, 2976-2979; script.cpp:805-806                                                                  |
| ...\plugins\                             | user plugin DLLs                                                                                                                    | plugins.cpp:34                                                                                                   |
| ...\minidumps\                           | crash zips and redirected game minidumps                                                                                            | exception.cpp:199-202, 561-573, 873                                                                              |
| %LOCALAPPDATA%\EZZ BOIII\WebView2        | WebView2 user data                                                                                                                  | html_frame.cpp:49-60                                                                                             |
| %TEMP%                                   | boiii-tlsdll.dll; `ezz-*` minidump temp files                                                                                       | tls.cpp:12; minidump.cpp:21                                                                                      |
| <game>\boiii_players\                    | per-user config (binds, config.cfg, friends, profile, server lists, properties.json); the game's players/ folder is redirected here | path.cpp:29-33, 127; properties.cpp:31-43; binds.cpp:37; dvars.cpp:95; server_list.cpp:151-232                   |
| <game>\boiii\                            | scripts, ui_scripts/lobby_scripts, scriptdata, gamesettings, plugins, data/lookup_tables                                            | script.cpp:807, 1058, 1243; ui_scripting.cpp:1038-1040; gsc_funcs.cpp:171; game_settings.cpp:115; plugins.cpp:37 |
| <exe dir>\logs\                          | boiii_lua_errors.log                                                                                                                | ui_scripting.cpp:1704-1708                                                                                       |
| <game>\versions\                         | boiii-beta.exe, boiii-<tag>.exe                                                                                                     | main.cpp:929; launcher.cpp:1328-1340                                                                             |
| %LOCALAPPDATA%\Activision\CoD\config.ini | created empty by the updater if missing                                                                                             | file_updater.cpp:162-187                                                                                         |

Game-folder resolution order (main.cpp:694-893):

1. The cwd, if it contains BlackOps3.exe or the server exe.
2. game_path.txt.
3. find_steam_game_path:
   - the default path <local scratch copy, not kept>\Program Files
     (x86)\Steam\steamapps\common\Call of Duty Black Ops III;
   - the Steam install path, read via steam::SteamAPI_GetSteamInstallPath from
     the registry (steam.cpp:131-140);
   - libraryfolders.vdf.
4. The WebView2 "BOIII - Game Setup" folder picker.

The result is then applied with SetCurrentDirectoryA.

**Can boiii.exe live outside the game folder? Yes, today.** The cwd moves to the
game folder first, every user path is relative to the cwd, and
LoadLibraryA("BlackOps3.exe") finds the exe through the DLL search order:
boiii.exe's own folder, then the system folders, then the cwd. The search-order
detail is inferred. Caveats:

- **Bug.**
  `main.cpp:649-651: if (!is_valid_game_folder(default_path)) { return std::string(default_path); }`
  returns the default path when it is NOT valid.
  - resolve_game_path then reports success, and main's retry loop
    (main.cpp:1030-1036) repeats the same steps forever.
  - The failing case: a game installed outside the default location, launched
    from outside the game folder, with no valid game_path.txt. The program hangs
    instead of scanning the Steam libraries or showing the picker.
  - It works on this PC only because the default path is valid.
- The game's imports are resolved with plain LoadLibraryA(name)
  (loader.cpp:51-52). A DLL with the same name next to boiii.exe would win over
  the game folder's copy (inferred from the search order).
- base.cpp:13-14 still tells the user to "place Boiii.exe next to
  BlackOps3.exe".
- File I/O redirection: patches.cpp:437-524 detours _fsopen, _wfsopen, _mkdir,
  _stat64, _stat64i32 and FS_BuildOSPath through path::normalize
  (path.cpp:97-149). The rules are:
  - players becomes boiii_players;
  - \LPC becomes <game>\LPC;
  - workshop content goes to usermaps/mods when there is no Steam workshop
    folder.

## (e) Logging

| Output               | Builds                       | Location                                                                                                   | Format and notes                                                                                                                                                              | Code                                                          |
| -------------------- | ---------------------------- | ---------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| game::trace / tracef | Debug only; no-op in Release | `-tracing <path>` (servers get a "-server" suffix), else <exe dir>\debug.log or debug-server.log, appended | `[<ctime>.<ms>][Debug] <msg>`; \n and \r escaped, non-printables dropped; written by a Logger thread from a moodycamel queue, flushed per message                             | game/log.hpp:16-105; game/log.cpp:12-46; game/flags.cpp:36-71 |
| console              | all                          | window "BOIII V<ver> - Console/Server", or stdout under -headless                                          | printf and the Com_Printf queue are hooked; -nologs disables the console patches; -fulllogs removes the buffer cap                                                            | console.cpp:1317-1345, 1580-1650                              |
| exception_log        | all                          | stdout / stderr                                                                                            | crash report lines; "Known crash patched" (debug only)                                                                                                                        | exception.cpp:37-49, 746, 802-845                             |
| crash zip            | client                       | %LOCALAPPDATA%\boiii\minidumps\ezz-crash-%Y-%m-%d-%H-%M-%S.zip                                             | crash.dmp plus info.txt ("Ezz Crash Dump", address, module+RVA, symbol/source, OS version, callstack, registers). The dialog opens Explorer on the folder unless -quiet-crash | exception.cpp:260-294, 366-374, 473-573                       |
| game minidumps       | all                          | redirected into ...\minidumps                                                                              | MiniDumpWriteDump detour                                                                                                                                                      | exception.cpp:180-220, 886-892                                |
| plugins log          | all                          | <game>\boiii_players\plugins.log (servers: plugins-server-<pid>.log), deleted at every start               | `[%Y-%m-%d %H:%M:%S] msg`                                                                                                                                                     | plugins.cpp:22-27, 80-100                                     |
| Lua errors           | all                          | <BlackOps3.exe dir>\logs\boiii_lua_errors.log                                                              | `[%Y-%m-%d %H:%M:%S] <LUI or LobbyVM error>`, then the stack                                                                                                                  | ui_scripting.cpp:1702-1721                                    |
| games_mp.log         | server                       | relative to cwd; g_log is now a string dvar                                                                | `%3i:%i%i <text>`                                                                                                                                                             | game_log.cpp:50-91                                            |
| OutputDebugString    | all                          | debugger only                                                                                              | updater and relaunch traces                                                                                                                                                   | file_updater.cpp; nt.cpp:300-315                              |

Release builds have no file log of loader or component activity.

## (f) Dedicated server and -headless

**Dedicated server.**

- Chosen by -dedicated, or automatically when only
  BlackOps3_UnrankedDedicatedServer.exe exists (main.cpp:1038-1039).
- Skips the synchronous updater, the launcher UI, exe patching and the GPU
  registry write (main.cpp:1045-1069).
- Runs the 9 server components plus the 49 generic ones. Server addresses come
  from the third symbol column, `_g` literals and the relocate tables.
- Hardening added for servers:
  - dedicated/networking.cpp: connection rate limiting, chat sanitising, a
    Cmd_ParseArgs RCE block and a TeamOps arbitrary-write fix
    (networking.cpp:23-330);
  - dedicated/info.cpp updates the console title every second.
- Console:
  - without -headless, boiii shows its own Win32 console window (class "BOIII
    WinConsole");
  - with -headless, output goes to the parent console or a newly allocated one
    (console.cpp:1580-1597);
  - -noconsole hides it (console.cpp:82).
- Not installed on this PC.

**-headless** (game/flags.cpp:98-101) does only the following:

- allocates or attaches a console and sends console text to stdout
  (console.cpp:1580-1597, 1646-1647);
- skips boiii's console window (console.cpp:1318-1320);
- show_error prints instead of opening a MessageBox (game.cpp:22-23);
- no exe-patch prompt (main.cpp:306);
- headless updater UI (updater_ui.cpp:12-13).

**-headless does NOT:**

- skip the WebView2 launcher (that needs -launch, main.cpp:1063-1066);
- skip the splash window (splash.cpp:143-146, no headless check);
- suppress the crash dialog and Explorer window (that needs -quiet-crash,
  exception.cpp:285-291);
- stop the client from creating its game window and D3D11 device (no renderer
  gating exists).

So the only windowless configuration today is a dedicated server with -headless.

**Indexer/test mode.** A client indexer would run with
`-launch -headless -quiet-crash -noupdate -noplugins`, do its work at the start
of post_unpack, write a report and exit. Whether that happens before the game
opens its window is not verified.

## (g) Plugin API (plugins.cpp)

- **Discovery:** every `*.dll` in %LOCALAPPDATA%\boiii\plugins, then
  `<cwd>\boiii\plugins` (cwd = game dir), in directory_iterator order
  (plugins.cpp:34-38, 102-142).
- **Timing:** LoadLibrary happens in the component constructor, i.e. during
  activate(), before the game is mapped. DllMain runs at that point.
- **ABI:** optional exports looked up by name, all cdecl with no arguments
  (plugins.cpp:49-74, 121):
  - `const char* p_name()`
  - `void post_load()`
  - `void post_unpack()`
  - `void pre_destroy()`
- **What is missing:** no ABI version and no host function table. There is no
  access to boiii's scheduler, commands, symbols or hook helpers. A plugin
  brings its own hooking and offsets; from post_load onward it can use
  GetModuleHandle(NULL) as the game base.
- **Gaps:**
  - library::invoke returns T{} for a missing export (nt.hpp:113-150), and the
    module handle is never checked. A plugin without p_name, or a DLL that fails
    to load, therefore reaches std::string(nullptr) at plugins.cpp:128. That is
    undefined behaviour, and an access violation there is not caught by
    catch(...) under /EHsc.
  - -noplugins is ignored for -dedicated (plugins.cpp:29).
  - The plugins folder in AppData is subject to updater cleanup.
  - Plugins run at the default `min` priority.
- **Second native-code path:** Lua mods can call package.loadlib after the
  unsafe-Lua dialog or -unsafe-lua; -disable-loadlib turns this off
  (ui_scripting.cpp:56-86, 1285-1348, 1769).
- **Verdict:** a dev/RE module can ship as a plugin DLL today, but it would have
  no ABI. A versioned Xerces plugin ABI is needed for anything maintainable.

## (h) Rename scope

Totals by area:

| Area        | Count                                                                                                |
| ----------- | ---------------------------------------------------------------------------------------------------- |
| src         | 151 lines containing "boiii" + 76 lines containing "ezz" (false positives excluded), across 57 files |
| data/       | 132 occurrences in 17 files (launcher UI and Lua UI scripts)                                         |
| README.md   | 43 lines                                                                                             |
| docs/       | 17 lines (asset-override.md 9, gsc-scripting.md 8)                                                   |
| .github/    | 38 occurrences in 5 files                                                                            |
| build files | premake5.lua 2, build.bat 3, generate.bat 1, package.json 2, scripts/unix/cross.sh 2                 |

| Category                                            | Representative refs                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Binary and build names                              | premake5.lua:260 `workspace("boiii")`, :442 `targetname("boiii")`; tls.cpp:12 boiii-tlsdll.dll; file_updater.cpp:19-25 (boiii.json, boiii-beta.json, UPDATE_HOST_BINARY boiii.exe); main.cpp:929; launcher.cpp:1332, 1450 (process check "boiii.exe"); utils/flags.cpp:13 `argparse("boiii")`; resource.r<local scratch copy, not kept>73-79; tlsdll/resource.r<local scratch copy, not kept>63; .github/workflows/build-and-publish.yml:26-28, 70, 96, 111-113; build.bat:11-26; package.json:2-3                                                                                                                        |
| Window titles, classes, UI strings                  | splash.cpp:57, 65; console.cpp:1326-1329, 1582, 1590; launcher.cpp:1283, 1366, 1473; main.cpp:276-310, 338, 423-471, 736, 1054; html_frame.cpp:305, 445, 451, 461; progress_ui.cpp:179, 197; progress_ui.hpp:36; updater_ui.cpp:29; dedicated/info.cpp:32, 40; branding.cpp:36 ("EZZ: <ver>" watermark), :42 ("EZZ> " console prefix), :56-57 (game window title prefix); exception.cpp:275, 285, 330, 343, 484, 570; nat.cpp:172; upnp.cpp:69; discord.cpp:236-258, 347; bots.cpp:38-51; base.cpp:14; utils/flags.cpp:152                                                                                                |
| Mutex and pipe names                                | properties.cpp:130 "boiii-properties-lock"; auth.cpp:89 "ezz-boiii-auth-key-lock" (fenced file). No named pipes.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| AppData                                             | game.cpp:42 and properties.cpp:117 (%LOCALAPPDATA%\boiii); html_frame.cpp:56 (EZZ BOIII\WebView2); minidump.cpp:21 temp prefix "ezz-"; exception.cpp:564 "`ezz-crash-*.zip`"                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| Game-folder names                                   | boiii_players: 30 refs in src (path.cpp:29-33, binds.cpp:37, dvars.cpp:95, friends.cpp:30, profile_infos.cpp:27, 208, server_list.cpp:151-232, bdStorage.cpp:171, properties.cpp:33, plugins.cpp:26, asset_limits.cpp:197, launcher.cpp x12). "boiii" folder: 11 refs (script.cpp:807, 1058, 1243; gsc_compiler.cpp:70; gsc_funcs.cpp:171; ui_scripting.cpp:1040, 1995; game_settings.cpp:115; plugins.cpp:37). Also boiii_lua_errors.log (ui_scripting.cpp:1708) and the .boiii_download / .boiii_backup suffixes (main.cpp:344-346)                                                                                     |
| URLs and hosts                                      | r2.ezz.lol (file_updater.cpp:17-23; main.cpp:935; main.js:4527); master.ezz.lol:20810 and m.ezz.lol:20810 (server_list.cpp:287-288); dc.ezz.lol (exception.cpp:275, 335, 348); forum.ezz.lol (discord.cpp:237; workshop.cpp:47-60, 14 links); ezz.lol (main.html:311, 965); api.github.com/repos/Ezz-lol/boiii-free/releases (main.js:4466); github.com/Ezz-lol/boiii-free (README; main.html:3326); archive.org exe pins (main.cpp:127-130; main.html:2782)                                                                                                                                                              |
| User agents                                         | http.cpp:124, 186, 231 "ezz-updater/1.0", sent on every utils::http request; fastdl.cpp:225 "fastdl/1.0"; a browser user agent for Steam Community HTML pages (workshop.cpp:654; launcher_workshop.cpp:441, 914)                                                                                                                                                                                                                                                                                                                                                                                                          |
| Registry                                            | nothing branded. main.cpp:605-619 writes HKCU\Software\Microsoft\DirectX\UserGpuPreferences with a value named after the exe's full path ("GpuPreference=2;")                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| External IDs                                        | Discord application id 967371125573177474 (discord.cpp:106)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| Compatibility identifiers (renaming breaks interop) | Engine.IsBOIII / Engine.IsEZZBOIII (ui_scripting.cpp:1024-1028); rich-presence keys boiii_invite (steam_proxy.cpp:570, 776) and boiii_game_info (friends.cpp:488, 559); Lua menus `Boiii*` and `CoD.__boiii*` (`data/ui_scripts/**`, toast.cpp:111-117); launcher localStorage keys boiii_launch_options and boiii_settings (main.js:516, 611, 3636, 3657, 4118); XUID key salts "ezz-boiii-auth-v2"/"ezz-boiii-auth-alias-v2" and "ezz-public"/"ezz-private" (auth.cpp:76-77, 93; fenced, and changing them changes every player's identity); user data in boiii_players and %LOCALAPPDATA%\boiii, which needs migration |
| Comments only                                       | cg/core.hpp:54-65; impl/cl/cl.cpp:172-243; quake/core.hpp:57, 66; level.hpp:46; snd.hpp:21; lua/cod/core.hpp:14; lualibs.cpp:9; client_patches.cpp:277; friends.hpp:45                                                                                                                                                                                                                                                                                                                                                                                                                                                    |

Protocol strings carry no branding: "T7" in heartbeat, getservers and gamename
(getinfo.cpp:99; server_list.cpp:350-351; dedicated/command.cpp:23). The license
is GPL-3.0 (LICENSE:1-2).

## (i) Fenced code

The full list is in off_limits. Some of it is wired into the core path:

- main.cpp's patch_imports installs the Steam emulation in the same function
  that installs the ExitProcess and SetProcessDPIAware hooks.
- One of the two post_unpack triggers is the Steam restart stub.
- The other trigger, SetProcessDPIAware, is independent of Steam and is imported
  by the installed client.

security.hpp is network-packet validation, not anti-tamper, and is not fenced.

## Design implications

- Keep the in-process loader (LoadLibrary mapping, manual imports, TLS helper
  DLL, PEB swap), but give Xerces explicit, documented phases: preload
  (constructor), image_mapped (post_load, IAT and OS hooks only),
  symbols_resolved (new, first step of post_unpack after the fenced arxan
  component), features (post_unpack), game_ready (scheduler dvars_loaded or
  first frame) and shutdown.
- Replace static-initializer ordering for Xerces components and mods with an
  explicit manifest or DAG (name, depends_on, priority), and log the resolved
  order at startup. Keep upstream's enum only for the existing fenced
  high-priority components.
- Build a data-driven symbol registry. Generate it from `game/symbols/**` plus
  the inline select, _g, relocate and offset sites. Key it by exe identity
  (CheckSum + TimeDateStamp + SizeOfImage + SHA-1), seed it with the three
  existing columns, and fall back to signatures (plus offset or RIP-relative
  xref extraction for data and patch sites) for unknown builds. `symbol<T>`
  carries only an id, and get() reads a pre-relocated table.
- Resolve all symbols once at the start of post_unpack and cache them in a
  Xerces-owned directory outside anything the updater cleans. Validate every
  entry: in the expected section, unique signature match, agreement with the
  known address. Fail with a clear per-symbol report instead of a wild pointer.
- Write a section-scoped, multi-pattern scanner rather than reusing the
  whole-image linear one. The first .text (47 MiB) holds all 3,200 named code
  symbols. Accept both IDA-style '??' and single '?', and keep it independent of
  the patterns in the fenced arxan.cpp.
- Add a headless indexer and test mode on top of -launch -headless -quiet-crash
  -noupdate -noplugins. A Xerces flag would run symbol resolution and self-tests
  at the start of post_unpack, write a JSON report and ExitProcess. The server
  column needs the dedicated server exe (not installed here).
- Make version policy data-driven: supported builds come from the registry, not
  constants in main.cpp. Do not auto-download game binaries from third parties
  or delete the user's original exe; detect the build, report it, and point
  users to official Steam mechanisms.
- Create one paths module (Xerces roots plus a read-only fallback and migration
  from %LOCALAPPDATA%\boiii and boiii_players). Load BlackOps3.exe and its
  imports by absolute path, fix the inverted find_steam_game_path check, and
  replace base.cpp's 'place Boiii.exe next to BlackOps3.exe' message.
- Repoint the updater to a Xerces release channel or disable it until one
  exists, as the fork did in e5968d07. Change cleanup to delete only files the
  updater itself installed, never unknown user files, plugins or mod content.
- Define a versioned plugin ABI v1: a C query export returning name, version and
  api_version, and a host table with logger, scheduler, command registration,
  symbol lookup and hook helpers. Load plugins in deterministic sorted order,
  honour -noplugins everywhere and guard a null p_name. Ship the fork's RE
  tooling (HW breakpoints, disassembly, histograms) as a dev-only plugin rather
  than in release builds.
- Add a release-capable structured logger: a per-session file in a Xerces logs
  directory with channels for loader, symbols, mods, conflicts, net and plugins.
  Keep the debug trace format for developer builds, and route plugin and Lua
  error logs through it.
- Do the rename in two tiers. Brand tier: exe and workspace names, titles,
  window classes, URLs, user agent, Discord app id, WebView2 folder, crash file
  names, CI, README. Compatibility tier, kept or aliased:
  Engine.IsBOIII/IsEZZBOIII alongside a new Engine.IsXerces, rich-presence keys,
  the 'T7' master protocol, migrated user-data folders. The XUID salts in
  auth.cpp stay untouched because the file is fenced.
- Keep the fenced code physically separated (e.g. untouched files, or a
  dedicated folder excluded from Xerces edits), and make sure Xerces features
  never call into it. The core lifecycle can rely on the SetProcessDPIAware
  post_unpack trigger, which the installed exe imports and which does not
  involve Steam.
- Use import-level hook points for the DX11 work. The game imports
  D3D11CreateDevice and CreateDXGIFactory1 directly, and download_overlay.cpp
  already hooks IDXGISwapChain::Present and draws ImGui, so device, swapchain
  and upscaler interception needs no game offsets.

## Risks

- The address tables tie the code to exactly two client builds plus one server
  build. When Steam ships a new client, upstream's only remedy is replacing the
  exe from archive.org. Until signatures exist, Xerces inherits the same
  breakage on every game update.
- The DeroXP fork's PoC addresses target 0x888C368 (upstream's legacy build),
  not the installed 0x6531394 build. Porting them needs translation, for example
  via upstream's legacy column where a named function matches, or via
  signatures.
- The upstream updater would undo Xerces. A Release Xerces build launched
  without -noupdate is replaced by upstream's boiii.exe from r2.ezz.lol. Each
  update check also deletes non-manifest content: %LOCALAPPDATA%\boiii\plugins,
  %LOCALAPPDATA%\boiii\minidumps and user-added files under data/
  (file_updater.cpp:637-713). Whether the live manifest protects plugins/ is
  unverified.
- Several code paths download and execute binaries from upstream infrastructure:
  -beta (main.cpp:924-990), the launcher version picker (main.js:4463-4530;
  launcher.cpp:1315-1355) and the archive.org exe replacement
  (main.cpp:127-130). The exe replacement deletes the user's original exe after
  success and redistributes a copyrighted binary from a third-party host.
- Most components run in link order (static initializers), which conflicts with
  the deterministic load-order goal. Naively adding mod-system components
  reproduces the problem.
- The inverted check in find_steam_game_path (main.cpp:649-651) can hang startup
  forever for non-default library locations. Bare LoadLibraryA names let a
  same-named DLL next to boiii.exe win over the game folder's copy.
- Field diagnostics are weak: tracing, component names and the log component
  exist only in debug builds. Release builds have no loader or component log to
  explain mod conflicts.
- The plugin ABI is fragile: there is no version or host API, and a missing
  p_name export or failed LoadLibrary hits a null std::string. -noplugins is
  ignored on servers. Plugins load before the game is mapped with no isolation.
  Lua package.loadlib is a second native-code path.
- Flags mislead users: -safe claims to disable mods but does not, and -plugins,
  -trimlogs, -keep-launcher and -port are no-ops.
- The 26 known-crash fixes in exception.cpp apply only to the legacy client. The
  installed build gets none of them.
- Three malformed address literals (console.hpp:10-11, quake.hpp:17-18,
  db/core.hpp:10) produce wild pointers on the server and legacy paths if those
  symbols are used.
- Using the existing scanner naively, i.e. a whole-image linear scan of about
  471 MiB per pattern for about 3,400 symbols, would make startup prohibitively
  slow without section scoping, batching and caching.
- Renaming compatibility identifiers breaks interop with upstream clients and
  existing mods: Engine.IsBOIII, the boiii_invite and boiii_game_info
  rich-presence keys, `Boiii*` Lua menu names, and the boiii_players and
  %LOCALAPPDATA%\boiii data locations. The XUID salts in the fenced auth.cpp
  change identities if touched.
- Fenced code sits on the core path: main.cpp's patch_imports and one
  post_unpack trigger belong to the Steam emulation. Refactoring main.cpp or the
  lifecycle without touching fenced functions needs care.
- -headless is not windowless for the client: the launcher appears unless
  -launch is passed, the splash and game windows open, and the crash handler
  opens Explorer unless -quiet-crash. An unattended indexer could still pop up
  UI or hang.
- With crash dialogs suppressed (quiet-crash), the scheduler async thread
  already runs between post_load and post_unpack. Any Xerces async task that
  touches game memory early will fault.

## Open questions

- Does the client create its game window or D3D11 device before the
  SetProcessDPIAware / SteamAPI_RestartAppIfNecessary trigger? This decides
  whether an indexer can exit from post_unpack without ever showing a window.
- Which post_unpack trigger fires first on the client, and which one fires on
  the dedicated server? The server exe is not installed here, so its imports
  could not be checked.
- Does the live r2.ezz.lol boiii.json manifest list plugins/ or minidumps/? If
  not, upstream's updater deletes user plugins and crash dumps on every launch.
- How stable are signatures between builds? Measuring the share of the 3,200
  named code symbols with unique signatures that resolve correctly on both
  0x888C368 and 0x6531394 needs the legacy exe, which is not installed.
- Product and legal decision: does Xerces keep the inherited Steam emulation and
  ownership-bypass path unchanged (fenced), or require a genuine Steam-owned
  copy? The answer changes which post_unpack trigger and Steam API path Xerces
  relies on.
- Is the TAC (Treyarch anti-cheat) circumvention inside the user's 'anti-tamper'
  boundary? It is fenced conservatively here, but GSC hook/override work touches
  neighbouring code in gsc_funcs.cpp and vm.cpp.
- Should Xerces keep the launcher in-process (WebView2 inside boiii.exe with
  self-relaunch on -launch), or split it into a separate launcher exe that
  starts the game host?
- Is the DLL search-order concern real for installs where boiii.exe sits outside
  the game folder, e.g. a stray steam_api64.dll next to it? It needs a quick
  test with absolute-path loading.
- The game writes '<module>.start' crash markers, and remove_crash_file targets
  boiii.start. Does running from a renamed exe change the game's safe-mode and
  crash-marker behaviour?

## Fenced (do not modify)

- src/client/component/arxan.cpp (whole file; priority arxan; post_load at 766,
  post_unpack at 843): Arxan anti-tamper handling. It also implements the -dump
  writer for the post-unpack executable image (arxan.cpp:768, 853).
- src/client/component/integrity.hpp: table of 1,223 Arxan integrity-check block
  addresses, used only by arxan.cpp:15.
- src/client/game/symbol.hpp:6-9 and 69-73 (symbol<T(Args...)>::call_safe), plus
  src/client/game/game.hpp:14-17 (arxan::detail::set_address_to_call,
  callstack_proxy_addr): route game calls through the Arxan callstack proxy.
  Used at game/utils.hpp (2 sites) and fov.cpp (1 site).
- The six pattern scans at src/client/component/arxan.cpp:636-696: the scanner
  in `src/common/utils/signature.*` is generic, but these patterns belong to the
  anti-tamper code; do not reuse or extend them.
- src/common/utils/flags.cpp:169-173 (-dump flag): writes the game or server
  executable after Arxan unpack; handled inside arxan.cpp.
- src/client/main.cpp:37-50 and 77-104 (patch_steam_import and the steam_api64
  IAT redirection in patch_imports), plus 52-70 (restart_app_if_necessary_stub):
  replace the game's Steam API, including its restart-through-Steam check, with
  boiii's emulation.
- src/client/main.cpp:545-562 handle_process_runner (-proc <pid>): receiving end
  of steam_proxy's Steam SpawnProcess launch under another app id.
- `src/client/steam/**` (Steam API reimplementation that answers license and
  ownership queries). steam.cpp:92: SteamAPI_RestartAppIfNecessary returns
  false. interfaces/apps.cpp:8-41, 96: BIsSubscribed, BIsSubscribedApp,
  BIsDlcInstalled. interfaces/user.cpp:104: auth ticket 'BOIII'.
  interfaces/game_server.cpp:84-98: GetAuthSessionTicket, BeginAuthSession,
  UserHasLicenseForApp.
- src/client/component/steam_proxy.cpp, ownership and app-id parts: 37-38
  (launch_app_id 480); 96-117 (ownership_state, -nosteam is_disabled,
  SetAppIDForCurrentPipe); 175-225 (client-engine load and SteamAppId env);
  270-330 (start_mod, start_mod_unsafe, SpawnProcess, evaluate_ownership_state);
  338-349 (post_load and post_unpack wiring). Ownership evaluation and app-id
  spoofing.
- src/common/utils/flags.cpp:86-89 (-nosteam, help text 'Bypass Steam
  entirely'): consumed at steam_proxy.cpp:103-106.
- src/client/component/auth.cpp (whole file). post_unpack (628-745) patches
  LiveSteamAuth_Pump, LiveSteamServer_GetAuthDataById, EndAllClientAuthSessions,
  LiveSteamLobby_GetAndValidateID and steam-id checks; lines 32-200 hold the
  identity key derivation. This is Steam auth-session and steam-id validation
  bypass plus player identity. Its salt strings (76-77, 93) are also in the
  rename scope but must not be changed.
- src/client/game/symbols/live/steam/auth.hpp and `live/steam/server/*.hpp`:
  declarations of the Steam auth functions that auth.cpp patches. Reading is
  fine; fence edits.
- `src/client/game/demonware/servers/auth3_server.*`: emulated DemonWare auth
  service, the online-auth step that normally validates the platform ticket.
- src/client/component/demonware.cpp:427-438: rewrites the auth URL to
  http://%s:%d/auth/ and skips publisher file signature checks (436-438).
  Authentication and content-signature verification bypass inside the DemonWare
  emulation.
- `src/client/game/demonware/services/bdAnticheat.*` and
  src/client/component/live.cpp:220-222 (LiveAntiCheat_ConsoleDetailsReported
  hooked to return_true): anti-cheat reporting stubs.
- Treyarch anti-cheat (TAC) circumvention: client_patches.cpp:264-320
  (store_tac_protected_allocs); game/impl/cg/cg.cpp:55-80 (static pools
  replacing TAC-protected allocations); game/symbols/cg/core.hpp:52-75 and
  level.hpp:18 (builtin pool and g_entities_cl symbols);
  gsc/gsc_funcs.cpp:286-380 and gsc/vm.cpp:217-218 (re-implementations that
  avoid TAC-protected access). Circumvents memory protection, adjacent to
  anti-tamper; fenced pending a decision.
- README.md:37-49: upstream's stated purpose ('removes Steam ownership
  verification', 'No Steam ownership required'). Xerces docs and branding must
  not carry these claims.
