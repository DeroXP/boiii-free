# Proof-of-concept findings (DeroXP fork, 17 commits, 2026-05-14 to 16)

> Machine-generated research notes from 2026-11-04, kept as evidence for
> `docs/DESIGN.md`. Code was read at upstream `main` 5aa7fac, `mod-stack`
> ac356c3c and the fork `refs/heads/main` 7805319. `path:line` citations refer
> to those commits. Treat claims as leads: anything marked runtime still needs
> the real game.

## Summary

The PoC is 17 DeroXP commits (2026-05-14..16) on top of upstream d80d308. They
change 18 files (+5027/-9). Most of the work is in workshop.cpp (+2819), plus
new perf.cpp, engine_perf.cpp/.hpp and console_logger.cpp, a Com_Error change in
patches.cpp, FS tracing in client_patches.cpp, and five new `tools/*.py`. The
fork registers 64 `bundle_*` console commands: 35 in workshop.cpp, 10 in
perf.cpp and 19 in engine_perf.cpp. Two more commands exist only in the file
watcher. On top of the commands, about 15 hooks and patches are installed on
every launch. These include an unconditional 1-byte patch that skips the 'Could
not find zone' Com_Error, global CreateThread/NtCreateThreadEx hooks, a
Scr_LoadScript detour that prints every call, and an always-on command-file
watcher. Every address is hardcoded to the exe that upstream now calls the
legacy client (PE CheckSum 0x888C368). The installed BlackOps3.exe is upstream's
new client: CheckSum 0x6531394, TimeDateStamp 2026-08-11, verified from the PE
header. Nothing in the PoC is pattern-scanned, and several patches would corrupt
code on any other build: bundle_perf_fps_unlock writes even when the byte it
finds is unexpected.

What the PoC established at runtime, per commit text only (no dumps or logs
survive locally):

- Calling loadMod twice is not additive.
- A workshop folder can be added to the live FS search path, and the list can be
  reordered.
- Calling DB_LoadXAssets directly works, while hooking its prologue failed to
  boot.
- Mod B's en_core_mod/core_mod can hot-swap mod A's in tier 0x40.
- Loading into a new tier fails at the en_ language-pair expansion ('Could not
  find zone en_X'; passing en_zm_mod produced en_en_zm_mod).
- Same-named zones in different slots trip a PMem LIFO fatal. The fork
  suppresses that error globally by returning from the Com_Error detour without
  calling the original.
- Perf: the engine is CPU-bound near 200 FPS on the user's rig. Sleep, Wait,
  Present-interval, draw-count, Map and RSL experiments were all null results. A
  hidden FPS-cap function was found and bypassed with a 1-byte jge->jmp patch.
- Crash lessons: printing before the console is ready, hooking Sleep or
  DB_LoadXAssets, and applying priority/timer changes at post_load all crashed.

Upstream's symbol tables are {new client, legacy client, server}, so most core
PoC addresses map exactly to the current exe (table in details). Three PoC
interpretations should be revised. First, the 'autoexec queue / runner /
queue-add' machinery (old 0x1432E6000, 0x1412D7F10, 0x1412D8810) sits inside the
script-string (`SL_*`) function cluster. Its 'export table base' 0x1450DC2D0 is
exactly upstream's gScrMemTreePub. Its Begin/End flag
`base+0x4D4E5CC+inst*0x820` is exactly gScrAnimPub[inst].animtree_loading. Its
0x10000/0x20000/0x30000 bits match the RefString user byte. So the
'BREAKTHROUGH' function is very likely a string-table routine, and calling it to
register autoexec exports would corrupt script strings. Second, the
'script_parse_tree pool list head' 0x149877B70 is an XAssetEntry slot inside
g_assetEntryPool, not a static global. Third, the 'arxan-protected secondary cap
at 0x1407CFD0' is upstream's Com_FPSLimit at 0x140F7CFD0, which upstream already
hooks to return UINT32_MAX.

Upstream already overlaps much of the PoC's goal. The working tree has a
re-implementation of UGC_LoadModByPublisherId with per-mod asset-pool config,
RAWFILE/SCRIPTPARSETREE overrides with mod- and map-specific trees, and
custom_scripts trees per mode/map/mod. It also has a GSC compiler with
detour/replacefunc (single replacement), the Com_FPSLimit fix, a scheduler
detour on Com_Frame_Try_Block_Function, and a single Present hook in
download_overlay. More importantly, upstream has an unmerged branch
upstream/mod-stack (c6676a31 + ac356c3c, 2026-10-15, 208 commits ahead of the
working tree) that implements additive multi-mod and avoids the PoC's wall. It
works as follows:

- It reads an fs_extramods priority list.
- Extra zones load as stack_<id>_<zone> in the same tiers (0x40 core, 0x100
  mode).
- It hooks the call sites inside DB_LoadXAssets that call DB_ExpandZoneList and
  DB_LoadXZone, and routes files through FS_BuildOSPathForThread.
- It resolves asset priority through DB_OverrideAsset and tracks owners in
  DB_LinkXAssetEntry.
- Each mod's entry script (such as _clientids.gsc) runs as a renamed copy,
  precache lists are merged, and servers advertise modStack. It does all this
  without PMem suppression or the validator patch, and returns early on the
  legacy client.

For Xerces, almost nothing should be ported verbatim:

- Production-worthy primitives are the zone table reader, script inventory, FS
  search-path diagnostics with correct upstream names, a DB_LoadXAssets batch
  wrapper for aliased same-tier loads, a shared Present dispatcher, opt-in perf
  settings applied after the main menu, and a console log tee with a
  multi-subscriber interceptor.
- The RE tools (disasm, find string/xref/dataref, dumps, profilers) belong in a
  dev-only module.
- Out of scope: the HW-breakpoint and thread-creation hooks (fenced as
  anti-tamper-adjacent), Com_Error suppression, the validator patch,
  skip-unload, inject_zone, script activation/extraction, the queue-add path and
  the FPS byte patch. Re-verification needs a live-process memory dump, because
  the client's on-disk .text is encrypted (commit 3b60fc6). The PoC's Python
  tools are a usable start. They need chunked reads, a process-name parameter,
  exclusion of the second .text and .interpr sections, and capstone-accurate
  xrefs, and they should emit signatures that resolve at post_unpack with
  upstream's utils::hook::signature, scanning only the first .text.

## Facts

- **verified**: The PoC is 17 DeroXP commits (2026-05-14..16) on upstream
  d80d308 and changes 18 files (+5027/-9). The bulk is workshop.cpp (+2819),
  engine_perf.cpp (+826, new), perf.cpp (+382, new) and five new `tools/*.py`.
  (git log refs/heads/main --author=DeroXP; git diff --stat d80d308
  refs/heads/main)
- **verified**: The fork registers 64 `bundle_*` commands (35 workshop.cpp, 10
  perf.cpp, 19 engine_perf.cpp). Two more exist only in the file watcher:
  bundle_sleep_watcher and bundle_dump_zones_now. (command::add grep:
  workshop.cpp L2029-3701, perf.cpp L228-367, engine_perf.cpp L511-808; watcher
  specials workshop.cpp L1709, L1792)
- **verified**: All PoC addresses target the exe upstream now calls the legacy
  client (CheckSum 0x888C368). The installed exe is upstream's new client.
  (d80d308:src/client/game/game.cpp is_client()==0x888C368; upstream
  src/client/game/base.cpp:36-48; installed BlackOps3.exe PE header read here:
  size 106059256, TimeDateStamp 0x6A7B6355 (2026-08-11 18:00:53 UTC), CheckSum
  0x6531394)
- **verified**: Upstream symbol initializers are {new client, legacy client,
  server}. The legacy column therefore equals the PoC addresses and gives an
  exact old-to-new map wherever upstream has the symbol.
  (src/client/game/ptr.hpp:62-84, symbol.hpp:14-41; e.g.
  DB_LoadXAssets{0x1414236C0, 0x1414236A0, 0x1401D8740} at
  symbols/db/load.hpp:18-20 vs fork symbols.hpp:92-94 (0x1414236A0))
- **verified**: No PoC address is pattern-scanned. All are _g literals or
  base+(static-0x140000000) arithmetic. (fork
  workshop.cpp:1908,1939-1951,3370,3408,3445,3800-3819; perf.cpp:234;
  engine_perf.cpp:282)
- **verified**: patches.cpp returns from the Com_Error_ detour without calling
  the original in two cases. 'Could not load default asset' is suppressed only
  while bundle_skip_unload is on. 'PMem_Free' + 'not at the top of the stack' is
  suppressed unconditionally.
  (refs/heads/main:src/client/component/patches.cpp:285-308; detour created at
  :355 on Com_Error_ (old 0x1420F8170); gate workshop.cpp:3844-3846)
- **verified**: Every launch, the fork patches BlackOps3.exe+0x1425169 from 0x75
  (jnz) to 0xEB to skip the 'ERROR: Could not find zone' Com_Error. The only
  guard is a 1-byte check that the original byte is 0x75.
  (workshop.cpp:1899-1931, called from post_unpack at :1936)
- **verified**: Slot redirection was never implemented in committed code. The
  detour on 0x142179960 returns the real value and only logs 40 calls. The
  'returns 22/23' behavior exists only in a comment. (workshop.cpp:1579-1599 and
  3136-3148; git log -G shows only d34d95d touched this code)
- **verified**: The PoC's names for two FS functions differ from upstream. PoC
  'FS_AddSearchPath' 0x1422A28D0 is upstream FS_AddGameDirectory(path, dir,
  bLanguageDirectory, iLanguage), new 0x142245DB0. PoC/fork-base
  'FS_AddGameDirectory' 0x1422A2AF0 is upstream FS_AddLocalizedGameDirectory,
  new 0x142245FD0. bLanguageDirectory explains the PoC's 'flag1 toggles a
  localization branch'. (upstream symbols/fs.hpp:35-39; fork
  workshop.cpp:3442-3450; commit ee8bcb3)
- **verified**: PoC 'function_4400' 0x1422A4400 is upstream FS_Startup (new
  0x1422478E0). The PoC's 'FS_Startup boundary 0x1422A4280-0x1422A45F0' is
  therefore wrong at its start. (symbols/fs.hpp:40-41; commit 88912f5 message)
- **verified**: Upstream names and new addresses for the PoC's mod-loading
  symbols:
- loadMod 0x1420D6930 = UGC_LoadModByPublisherId, new 0x1420CA1B0.
- inner_loadMod 0x1420D6B50 = UGC_LoadMod(lc, `WorkshopData*`, reloadFS), new
  0x1420CA3D0.
- isModLoading 0x1420D6DE0 = UGC_ModLoad_TryComplete, new 0x1420CA660.
- The active-mod global is ug<local scratch copy, not kept>:active_mod at legacy
  0x15678CCA0, new 0x15670DD20. The commit's '~0x14878CCA0' is a digit typo.
  (symbols/ugc.hpp:17-30; commit 88912f5)
- **verified**: The PoC's per-instance 'Begin/End flag' at
  `base+0x4D4E5CC+inst*0x820` is exactly
  gScrAnimPub.instance[inst].animtree_loading. gScrAnimPub is at legacy
  0x144D4DDB0 and the field is at +0x81C of the 0x820-byte scrAnimPub_t. New
  address: 0x144CCF5CC. (symbols/scr/vm/anim.hpp:9-10;
  structs/scr/vm/anim.hpp:12-23; fork workshop.cpp:118-121)
- **verified**: The PoC's 'runtime export table base' 0x1450DC2D0 is exactly
  upstream gScrMemTreePub, new 0x14505D2D0. It is an 8-byte pointer to the
  script memory-tree buffer, through which upstream resolves ScrString_t to
  RefString. (symbols/scr/mt.hpp:17-18; structs/scr/mt.hpp:107-116;
  impl/sl/sl.cpp:14-15)
- **verified**: All PoC 'autoexec queue' code lies inside the script-string
  `SL_*` cluster. This covers the 22 head-load sites 0x1412D738C-0x1412D8B9C,
  the runner 0x1412D7F10, queue-add 0x1412D8810 and the write at 0x1412D8BC9.
  Neighbors are SL_ConvertToStringSafe 0x1412D7180, SL_GetString 0x1412D7B20,
  SL_RemoveRefToStringFree 0x1412D7E90 and Scr_SetString 0x1412D8490. (upstream
  symbols/sl.hpp:7-16, scr/core.hpp:83-84; anchor computation (scratchpad
  anchor_deltas.py))
- **likely**: The 'autoexec queue' is very likely the script string table, not
  an autoexec registry. Calling the 'queue-add' function to register autoexec
  exports would therefore write into the string table. The reasons:
- The bits the PoC labeled PRIVATE/AUTOEXEC (0x10000/0x20000, mask 0x30000)
  match the RefString 'user' byte at bits 16-23 and CoD's script-string hash
  status bits.
- The file-format export flags are different values: EXPORT_AUTOEXEC=2 and
  EXPORT_PRIVATE=4.
- The table is indexed through gScrMemTreePub. (structs/sl.hpp:9-25;
  component/gsc/gsc_emitter.hpp:24-26; facts above on gScrMemTreePub and the SL
  cluster; commits d34d95d/3b60fc6/7805319)
- **likely**: PoC 'script_parse_tree pool list head' 0x149877B70 falls inside
  g_assetEntryPool, at entry 0x1D7FF offset +0x10. The pool starts at legacy
  0x1494C7B80 and holds 0x263FF entries of 0x20 bytes, ending at 0x14998FB60. So
  0x149877B70 is an XAssetEntry slot whose override chain uses nextOverride, not
  a static global. (symbols/db/xasset/pool.hpp:16-17;
  structs/db/xasset/pool.hpp:97-107; structs/db/xasset/core.hpp:968-977)
- **likely**: The 'arxan-protected secondary cap at 0x1407CFD0' is upstream
  Com_FPSLimit at 0x140F7CFD0, the same address in both builds. Upstream already
  hooks it to return UINT32_MAX, which makes the PoC's 1-byte limiter patch
  redundant. (symbols/com/core.hpp:86-90;
  component/client_patches.cpp:442,564-567; fork perf.cpp:215-224)
- **verified**: bundle_perf_fps_unlock writes 0xEB/0x7D at base+0x20F8F0E even
  when the current byte is neither value. It only prints a warning, so on any
  other exe build it corrupts a code byte. (fork perf.cpp:234-252)
- **verified**: Upstream already detours Com_Frame_Try_Block_Function (new
  0x1420EC680). The PoC's limiter jge was at +0x10E into that function (old
  0x1420F8F0E). (component/scheduler.cpp:274-275; symbols/com/core.hpp:76-77)
- **verified**: A second detour on the same target throws in
  utils::hook::detour::create, which confirms the PoC's Present-sharing
  constraint. Upstream still hooks IDXGISwapChain::Present only in
  download_overlay. (src/common/utils/hook.cpp:243-244;
  component/download_overlay.cpp:404-448)
- **verified**: The unmerged branch upstream/mod-stack implements additive
  multi-mod. It is c6676a31 'Add mod stacking' plus ac356c3c docs, by XFrozedyX
  on 2026-10-15, 208 commits ahead of the working tree. How it works:
- fs_extramods sets the stack order.
- Extra zones load as stack_<id>_<zone> in the same tiers (0x40 core, 0x100
  mode).
- It hooks the call sites at DB_LoadXAssets+0x347 (DB_ExpandZoneList) and +0x353
  (DB_LoadXZone).
- FS_BuildOSPathForThread routes files, DB_OverrideAsset sets priority and
  DB_LinkXAssetEntry tracks owners.
- It is disabled on the legacy client. (git show c6676a31 / ac356c3c;
  c6676a31:src/client/component/mod_stack.cpp:27-29,181-293,780-828; git
  rev-list counts (208 ahead, 0 behind))
- **likely**: Mod-stack's tier constants (0x40 core_mod, 0x100 mode mod) back up
  the PoC's zone tier table. They also show that the PoC's freeFlags 0x140 is
  core tier 0x40 plus mode tier 0x100, so bundle_load_ff's default
  freeFlags=0x100|alloc also frees the loaded zm_/mp_/cp_ mod tier.
  (mod_stack.cpp:28-29; fork workshop.cpp:2620-2631; commit 88912f5)
- **verified**: The upstream working tree already has, among other features:
- a UGC_LoadModByPublisherId re-implementation with per-mod asset pool config;
- RAWFILE/SCRIPTPARSETREE overrides with mod- and map-specific trees;
- custom_scripts trees per mode, map and mod;
- GSC detour/replacefunc, which replace a function outright with no chaining.
  (game/impl/ugc/ugc.cpp:276-323;
  component/script.cpp:733-893,1135-1167,1250-1266; docs/asset-override.md;
  docs/gsc-scripting.md:125-185)
- **likely**: Upstream's GSC compiler emits autoexec bootstrap functions into
  custom scripts, which contradicts the PoC's April finding that custom_scripts
  autoexec never fires. (component/gsc/gsc_compiler.cpp:524-530,740 (added
  0d4604b6, 2026-06-14); commit d34d95d test_autoexec.gsc claim)
- **verified**: The console interceptor is single-slot, and rcon's
  scoped_interceptor clears it. The PoC console_logger would stop logging after
  any rcon command. (component/console.cpp:1452-1458; console.hpp:11-22;
  rcon.cpp:42; fork console_logger.cpp:72-75)
- **verified**: The installed client has two .text sections plus .interpr:
- .text at VA 0x1000, size 0x2EFC600.
- .text at VA 0x1AA6B000, size 0x2CF0C00.
- .interpr at VA 0x2EFE000, size 0x9800. .data has VirtualSize 0x175D6360. The
  PoC states that the on-disk .text is encrypted, so static scanning needs a
  live-process dump. (PE section table read here; commit 3b60fc6 message
  (encryption claim))
- **verified**: No PoC runtime artifacts exist locally. tools/bo3_dump is
  gitignored and absent, and %LOCALAPPDATA%/boiii does not exist, so all runtime
  claims rest on commit text. (ls of Xerces/tools and LocalAppData; fork
  .gitignore adds tools/bo3_dump/)
- **unverified**: Hot-swap works. The sequence addpath, then promote_last, then
  bundle_load_ff core_mod 40 replaced mod A's en_core_mod/core_mod in tier 0x40
  with mod B's, with no crash. The same flow worked for zm_mod, cp_mod and
  mp_mod. (commit 88912f5 message)
- **unverified**: loadMod with reloadFS=false is not additive. Each call runs
  ShutdownGame + FS_Startup and rebuilds the search path with only the new mod.
  (commit e5968d0 message)
- **likely**: A path added via 0x1422A28D0 is always appended, and reordering
  the list works without a crash. The list head (old 0x157A65308) comes from the
  rip-relative load mov reg,[rip+0x157C29C6] at 0x1422A293B, which is
  FS_AddGameDirectory+0x6B. The candidate new head is 0x1579E6408, between
  fs_game (new 0x1579E63F8) and fs_loadStack (new 0x1579E6410). (commits
  ee8bcb3/9071412; fork workshop.cpp:3404-3408; symbols/fs.hpp:7,
  dvar/core.hpp:49)
- **unverified**: Loading into a new tier fails because DB_LoadXAssets
  unconditionally derives en_<name>: passing en_zm_mod produced en_en_zm_mod.
  The mod-stack symbol DB_ExpandZoneList (new 0x141426970) is the probable
  expander. (commit 88912f5; mod-stack symbols/db/load.hpp additions)
- **likely**: Detouring DB_LoadXAssets' prologue made the old game fail to boot,
  while calling it directly works. Mod-stack hooks call sites inside
  DB_LoadXAssets instead of the prologue. (fork client_patches.cpp:235-239,
  workshop.cpp:2485-2490; mod_stack.cpp:796-802)
- **unverified**: Perf results on the user's rig (RTX 5070 Ti, 24 cores):
- Default: 60 FPS with vsync.
- About 200 FPS with r_vsync 0, HIGH priority and 0.5 ms timer resolution.
- 210-300 FPS with the limiter patch plus Sleep skip.
- GPU about 58%; cutting surfaces by 66-75% gave only +4-5%.
- Present sync 0, Sleep skip, Map counting and Wait hooks gave nothing. The
  conclusion is a CPU-bound fixed per-frame cost. (commits 2ea1140, bfca64a,
  fd61f80, 9ecb1bd)
- **unverified**: Lifecycle crash lessons:
- Priority + timer + NtDelayExecution hook applied in post_load crashed at
  'Loading fastfile core_patch'.
- Sleep/SleepEx detours crashed even when logging only.
- printf from early threads or CreateFile hooks crashed.
- The CreateFileA detour crashed at boot.
- Calling 0x1412C7F20 directly raised an access violation.
  (bafcb8a:perf.cpp:222-230; commits 2211a93, 2ea1140, bfca64a; fork
  workshop.cpp:1667-1673,1836-1841,1855-1858; script.cpp:613-616)
- **unverified**: The HW breakpoint fired once, on the flag write at
  0x1412C7FC9. The queue-entry breakpoint never fired, even with 123 threads
  pre-armed and CreateThread/NtCreateThreadEx propagation. (commits d34d95d,
  4df651e, e3fadd0)
- **verified**: Upstream ships a generic signature scanner and a
  hardware_breakpoint utility. Their only consumer is component/arxan.cpp.
  (src/common/utils/signature.hpp, hardware_breakpoint.hpp; grep over
  src/client)
- **verified**: The 'Redundant asset' call site 0x141422AF2 is
  DB_LinkXAssetEntry (legacy 0x1414228C0) + 0x232. The PoC's 'Com_PrintWarning'
  0x142148F60 is upstream Com_Printf(channel, label, fmt, ...), new 0x1420F04A0.
  (symbols/db/load.hpp:21-23; symbols/com/core.hpp:27-29; fork
  workshop.cpp:100-105,3800)
- **likely**: The PoC's 'descriptor allocator / zone_lookup' 0x142179960 sits
  between Stream_CloseFile 0x142179810 and Stream_OpenFile 0x14217AF20, so it is
  a stream file-layer function. Candidate new address: 0x142120EC0.
  (symbols/stream.hpp:9-12 (both neighbors shift by -0x58AA0))
- **verified**: Code shifts legacy-to-new by region:
- DB/Scr 0x1412C7xxx-0x141425xxx: +0x20.
- UGC/Com 0x1420Dxxxx-0x1420Fxxxx: -0xC780.
- FS 0x1422Axxxx: -0x5CB20.
- Renderer 0x141C3xxxx-0x141D1xxxx: -0xC3D0.
- Com_Printf area: -0x58AC0. Most data shifts by -0x7F000 (FS globals by
  -0x7EF00). (computed from 3016 upstream symbols that have both addresses
  (scratchpad anchor_deltas.py))
- **verified**: The file watcher runs unconditionally. It executes every line
  written to %LOCALAPPDATA%/boiii/bundle_commands.txt as a console command,
  including bundle_patch, which is an arbitrary memory write. (fork
  workshop.cpp:1646-1829, 1955-1956, 3701-3757)
- **verified**: The fork forces CREATE_SUSPENDED on every
  CreateThread/NtCreateThreadEx call and resumes the thread afterwards, even
  when the queue hunt is off. (fork workshop.cpp:322-342, 351-380, 3784-3793)
- **verified**: The Scr_LoadScript detour prints every call and records caller
  return addresses unconditionally. The Begin/End detours also print every call.
  (fork workshop.cpp:117-127, 461-495, 510-559, 3805-3819)
- **verified**: bundle_extract_ds4c copies another mod's compiled script buffers
  to custom_scripts under renamed names, without patching imports. (fork
  workshop.cpp:3024-3110)
- **verified**: Upstream's integrity.hpp lists an anti-tamper site at
  0x1412D7E4C, 0xC4 bytes before the PoC 'runner' 0x1412D7F10. The list was only
  measured for distance, not analyzed. (src/client/component/integrity.hpp
  (addresses only))

## Details

# PoC multi-mod fork: catalog, findings, address tables

**Notation**

- `F:path:L` means
  `git -C <local scratch copy, not kept>ve/Desktop/Environment/Xerces show refs/heads/main:path`,
  line L.
- Line-identical copies of those files are in
  `<local scratch copy, not kept>--Users-Ginld-OneDrive-Desktop-Environment/dd57f53e-7df0-4e99-b301-986e5602a7c1/scratchpad/fork/`.
- `U:path:L` is the upstream working tree (`xerces` @5aa7fac).
- `MS:` is `upstream/mod-stack` @c6676a31/ac356c3c.

**Evidence tags**

- **RT**: runtime-observed, per commit text or code comments only.
- **DIS**: disassembly. The PoC used udis86 on live memory, or capstone on a
  memory dump.
- **INF**: inference.
- **VER**: verified here, in code, git or the PE header.

## 0. Provenance and the two exes

- **PoC exe.** Upstream's legacy client, PE CheckSum 0x888C368. The fork base
  `d80d308:src/client/game/game.cpp` `is_client()` tests 0x888C368, and
  `U:src/client/game/base.cpp:46-48` names it `is_legacy_client()`.
- **Current exe.** Read from the installed BlackOps3.exe header (VER):
  - size 106059256, TimeDateStamp 0x6A7B6355 (2026-08-11 18:00:53 UTC), CheckSum
    0x6531394. That checksum is `is_new_client()` (U:base.cpp:36-39).
  - Sections: `.text` VA 0x1000 size 0x2EFC600; `.interpr` VA 0x2EFE000 size
    0x9800; `.rdata` VA 0x2F08000; `.data` VA 0x3252000, vsize 0x175D6360 (raw
    0x3A3600); `.pdata`, `.tls`, `_RDATA`, `.rsrc`, `.reloc`, `.idata` (VA
    0x1AA67000); a second `.text` at VA 0x1AA6B000, size 0x2CF0C00.
- **Upstream symbol layout.** Symbols are written
  `symbol<T>{new_client, legacy_client, server}`, selected by checksum
  (U:src/client/game/ptr.hpp:62-84). The middle column therefore equals the PoC
  address.
- **No runtime artifacts survive.** `tools/bo3_dump/` is gitignored (fork
  `.gitignore` adds `release-staging/`, `*.zip`, `tools/bo3_dump/`,
  `tools/__pycache__/`) and absent. `%LOCALAPPDATA%\boiii\` (console.log,
  bundle_commands.txt) does not exist.
- **Co-authorship.** Commits d34d95d through 7805319 carry
  `Co-Authored-By: Claude Opus 4.7 (1M context)`, so part of the reasoning in
  the commit text is AI-written. Treat its interpretations as hypotheses.

## 1. Commit timeline (all `--author=DeroXP`)

| #   | Commit   | Date (-0500) | Subject                                                  | Files                                                                                   |
| --- | -------- | ------------ | -------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| 1   | e5968d07 | 05-14 14:47  | disable self-updater + bundle_test_loadmod               | updater.cpp (+4), workshop.cpp                                                          |
| 2   | ee8bcb30 | 05-14 18:08  | FS tracing + runtime disasm + search-path probes         | client_patches.cpp, console_logger.cpp (new), workshop.cpp                              |
| 3   | 9071412f | 05-14 19:22  | search-path manipulation + xref scanner + load_ff probe  | client_patches.cpp, workshop.cpp                                                        |
| 4   | 88912f57 | 05-14 20:17  | zone-loading commands + zone-tier wall documentation     | workshop.cpp                                                                            |
| 5   | d34d95d5 | 05-16 11:33  | autoexec dispatcher located, queue + runner mapped       | client_patches, patches (+29), script (+8), workshop (+1812), workshop.hpp, symbols.hpp |
| 6   | a999ae1f | 05-16 11:42  | gitignore release-staging/                               | .gitignore                                                                              |
| 7   | 4df651e3 | 05-16 12:37  | queue-write hunt (HW BP all-threads + caller histogram)  | workshop.cpp                                                                            |
| 8   | e3fadd0d | 05-16 16:48  | CreateThread + NtCreateThreadEx propagation for HW BP    | workshop.cpp                                                                            |
| 9   | bafcb8a4 | 05-16 17:04  | perf component (priority/affinity/timer + Sleep hunt)    | perf.cpp (new)                                                                          |
| 10  | 2211a934 | 05-16 17:18  | perf: defer step-1 to runtime commands (post_load crash) | perf.cpp                                                                                |
| 11  | 2ea11406 | 05-16 18:33  | strip Sleep-hook approach                                | perf.cpp                                                                                |
| 12  | bfca64a6 | 05-16 20:39  | FPS-cap unlock + D3D perf toolkit                        | download_overlay, engine_perf (new), perf (Sleep cmds re-added), workshop               |
| 13  | 00127fff | 05-16 21:21  | render-loop hot function                                 | engine_perf.cpp                                                                         |
| 14  | fd61f80e | 05-16 21:56  | RSL truncation, null result                              | engine_perf.cpp                                                                         |
| 15  | 9ecb1bdd | 05-16 22:01  | Map/Unmap hooks, final null result                       | engine_perf.cpp                                                                         |
| 16  | 3b60fc60 | 05-16 22:46  | queue-add found via memory dump + static scan            | tools/{dump_bo3_memory,scan_xrefs,scan_dump,find_queue_writer}.py, .gitignore           |
| 17  | 7805319a | 05-16 23:05  | queue-add verified via capstone                          | tools/disasm_func.py                                                                    |

`bundle_perf_sleep_ratio`, `yield_force` and `yield_delay` are named in
2ea1140's message but were never committed.

## 2. Command catalog

**Categories**

- **P**: production primitive worth porting. It must be re-implemented with
  signature-resolved addresses.
- **D**: RE/debug tool for a dev-only module.
- **X**: dead experiment.
- **F**: fenced (anti-tamper-adjacent); do not port.

All addresses are old-exe static addresses. "SPT" means script_parse_tree.

### workshop.cpp (35)

| Command                                                            | Args                                          | What it does                                                                                                                                                     | Old addresses / globals                                                                | Cat                               | F line                | Commit                  |
| ------------------------------------------------------------------ | --------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- | --------------------------------- | --------------------- | ----------------------- |
| bundle_test_loadmod                                                | mod [reloadFS]                                | Calls loadMod(LOCAL_CLIENT_0, mod, reloadFS), then polls isModLoading for up to 30 s.                                                                            | loadMod 0x1420D6930, isModLoading 0x1420D6DE0, getPublisherIdFromLoadedMod 0x1420D7680 | D                                 | 3498-3523             | e5968d0                 |
| bundle_disasm                                                      | hex [n=30]                                    | udis86 disassembly of live memory. Addresses in [0x140000000, 0x150000000) are rebased with game::relocate.                                                      | any                                                                                    | D                                 | 2029-2086             | ee8bcb3                 |
| bundle_dump_searchpath                                             | -                                             | Walks the list from head 0x157A65308. Each node is {next@0, data@8}; path is at data+0 and gamedir at data+0x100; max 50 nodes.                                  | 0x157A65308                                                                            | P (diagnostics)                   | 3398-3426             | ee8bcb3                 |
| bundle_addpath                                                     | basepath gamedir                              | Calls 0x1422A28D0(path, gamedir, 0, 0).                                                                                                                          | upstream FS_AddGameDirectory                                                           | P                                 | 3436-3453             | ee8bcb3                 |
| bundle_addpath_flags                                               | base gamedir f1 f2                            | Same call with flags. f1 is bLanguageDirectory and f2 is iLanguage.                                                                                              | same                                                                                   | D                                 | 3465-3491             | ee8bcb3                 |
| bundle_findxref                                                    | hex_runtime                                   | Scans executable regions for RIP-relative or rel32 references, using a prefilter plus udis86; capped at 25.                                                      | -                                                                                      | D                                 | 2282-2400             | 9071412                 |
| bundle_findstring                                                  | text...                                       | Scans readable regions for a literal string; capped at 10.                                                                                                       | -                                                                                      | D                                 | 2409-2483             | 9071412                 |
| bundle_load_ff                                                     | ff [alloc=0x800] [free=0x100\|alloc] [sync=1] | Calls DB_LoadXAssets(&zi, 1, sync, false) with allocSlot and freeSlot 0. Arms the redundant-asset capture during the call.                                       | DB_LoadXAssets 0x1414236A0                                                             | P (semantics must change)         | 2594-2670             | 9071412/88912f5/d34d95d |
| bundle_promote_last                                                | -                                             | Unlinks the tail node and makes it the head. No lock.                                                                                                            | 0x157A65308                                                                            | P (re-implement)                  | 3364-3391             | 9071412                 |
| bundle_dump_zones                                                  | -                                             | Prints g_zoneNames[0..63] (0x60 bytes each): name, flags, slot, state (-1..4).                                                                                   | 0x14998FB80                                                                            | P (diagnostics)                   | 2506-2547             | 88912f5                 |
| bundle_load_ff_pair                                                | base [alloc=0x800]                            | Calls DB_LoadXAssets with [{en_base, alloc\|0x8000000}, {base, alloc}], count 2, sync=true. Kept "as evidence"; it does not work.                                | 0x1414236A0                                                                            | X                                 | 2557-2592             | 88912f5                 |
| bundle_inject_zone                                                 | name flags_hex [slot=-1]                      | Writes a fake XZoneName into the first empty slot, with state=3 (COMPLETE).                                                                                      | g_zoneNames                                                                            | X                                 | 2102-2158             | d34d95d                 |
| bundle_dumpbytes                                                   | hex [n<=1024]                                 | Hex and ASCII dump. No rebasing.                                                                                                                                 | -                                                                                      | D                                 | 2166-2202             | d34d95d                 |
| bundle_finddataref                                                 | hex                                           | Scans for an 8-byte value in 4-byte steps; capped at 25.                                                                                                         | -                                                                                      | D                                 | 2210-2272             | d34d95d                 |
| bundle_activate_scripts                                            | [filter]                                      | Enumerates SPT assets with DB_EnumXAssets and calls Scr_LoadScript on each (server instance for .gsc, client for .csc).                                          | DB_EnumXAssets 0x141420970, Scr_LoadScript 0x1412C83F0                                 | X                                 | 2683-2734             | d34d95d                 |
| bundle_list_scripts                                                | [filter] [limit=200]                          | Lists SPT asset names.                                                                                                                                           | DB_EnumXAssets                                                                         | P (asset inventory) / D           | 2739-2785             | d34d95d                 |
| bundle_scr_loadscript                                              | 0\|1 name                                     | Calls Scr_LoadScript once.                                                                                                                                       | 0x1412C83F0                                                                            | D                                 | 2789-2810             | d34d95d                 |
| bundle_auto_activate_scripts                                       | 0\|1                                          | Turns the drain at begin_load_scripts on or off.                                                                                                                 | -                                                                                      | X                                 | 2818-2826             | d34d95d                 |
| bundle_dump_pending_activations / bundle_clear_pending_activations | -                                             | Inspects or clears the captured name set.                                                                                                                        | -                                                                                      | X                                 | 2830-2843 / 3016-3022 | d34d95d                 |
| bundle_dump_autoexec_list                                          | [bytes=0x200] [stride=0x10]                   | Raw and structured dump of `*(0x1432E6000)`, reading flags at +8 and index at +0xC.                                                                              | 0x1432E6000                                                                            | D (target misidentified, see F14) | 2978-3012             | d34d95d                 |
| bundle_extract_ds4c                                                | -                                             | Looks up each captured name with DB_FindXAssetHeader(SPT) and writes its buffer to `%LOCALAPPDATA%\boiii\data\custom_scripts\zm\<base>_ds4c<ext>`.               | DB_FindXAssetHeader 0x141420ED0                                                        | X                                 | 3042-3110             | d34d95d                 |
| bundle_trace_files                                                 | 0\|1                                          | Turns the CreateFileW trace filter on or off.                                                                                                                    | -                                                                                      | D                                 | 3115-3122             | d34d95d                 |
| bundle_skip_unload                                                 | 0\|1                                          | Gates three things: skipping DB_UnloadXZone for slots 15/16, skipping unload_ff_mapping for core_mod/en_core_mod, and suppressing the 'default asset' Com_Error. | 0x141425A70, 0x14141F040                                                               | X                                 | 3127-3134             | d34d95d                 |
| bundle_redirect_mod_slots                                          | 0\|1                                          | The comment says it redirects to slots 22/23. The code only logs 40 allocator calls.                                                                             | 0x142179960                                                                            | X                                 | 3140-3148             | d34d95d                 |
| bundle_load_solo_menu                                              | -                                             | SendInput sequence: Enter, wait 2.5 s, 5x Down, Enter, Enter.                                                                                                    | -                                                                                      | D (test automation)               | 3154-3206             | d34d95d                 |
| bundle_send_key                                                    | vk_hex [n] [ms]                               | SendInput scancode keys to the game window.                                                                                                                      | -                                                                                      | D                                 | 3212-3285             | d34d95d                 |
| bundle_dismiss_splash                                              | -                                             | SendInput Enter, as both VK and scancode.                                                                                                                        | -                                                                                      | D                                 | 3291-3353             | d34d95d                 |
| bundle_findstrxref                                                 | text...                                       | findstring (up to 8 matches), then one xref pass.                                                                                                                | -                                                                                      | D                                 | 3534-3689             | d34d95d                 |
| bundle_patch                                                       | hex b...                                      | VirtualProtect, arbitrary write, FlushInstructionCache.                                                                                                          | any                                                                                    | D (never in release)              | 3701-3757             | d34d95d                 |
| bundle_dump_scrload_callers / bundle_clear_scrload_callers         | -                                             | Top 20 Scr_LoadScript caller return addresses (static), or reset.                                                                                                | -                                                                                      | D                                 | 2865-2902             | 4df651e                 |
| bundle_arm_queue_bp_now                                            | [idx=0xC]                                     | DR0 write breakpoint on `*(0x1432E6000)+idx*8` in all threads.                                                                                                   | 0x1432E6000                                                                            | F                                 | 2911-2940             | 4df651e                 |
| bundle_hunt_queue_writer                                           | 0\|1 [idx]                                    | Re-arms the breakpoint in the Begin and LoadScript stubs and on new threads.                                                                                     | 0x1432E6000                                                                            | F                                 | 2952-2968             | 4df651e                 |
| bundle_dump_thread_hook_stats                                      | -                                             | Counters for the CreateThread and NtCreateThreadEx hooks.                                                                                                        | -                                                                                      | F                                 | 2850-2860             | e3fadd0                 |

**Watcher-only (handled on the watcher thread)**

- `bundle_sleep_watcher <ms>` (F:workshop.cpp:1709-1718)
- `bundle_load_solo_menu`, a watcher variant with an 8 s splash wait (1724-1785)
- `bundle_dump_zones_now`, which reads g_zoneNames while the main thread is busy
  (1792-1816)

All three are D.

### perf.cpp (10)

| Command                                                                  | Args                              | What it does                                                                                                                     | Cat                      | F line       |
| ------------------------------------------------------------------------ | --------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- | ------------------------ | ------------ |
| bundle_perf_fps_unlock                                                   | [1\|0] (no argument means unlock) | Writes 0xEB or 0x7D at base+0x20F8F0E (old 0x1420F8F0E). Writes even if the existing byte is unexpected.                         | X (superseded)           | 228-253      |
| bundle_perf_status                                                       | -                                 | Prints priority, affinity, timer resolution and sleep flags.                                                                     | P (perf panel)           | 255-276      |
| bundle_perf_priority                                                     | normal\|above\|high               | SetPriorityClass. Note that upstream NOPs the game's own priority change at U:client_patches.cpp:464-465.                        | P (opt-in, after menu)   | 278-292      |
| bundle_perf_timer                                                        | 0\|1                              | NtSetTimerResolution to the finest resolution (about 0.5 ms), or revert. Also reverted in pre_destroy.                           | P (opt-in, after menu)   | 294-308, 377 |
| bundle_perf_sleep_log / bundle_perf_sleep_dump / bundle_perf_sleep_reset | ...                               | Per-return-address Sleep histogram.                                                                                              | X                        | 310-328      |
| bundle_perf_sleep_skip                                                   | 0\|1 [ms]                         | Skips Sleep/SleepEx calls of up to ms when the caller is in BO3.                                                                 | X (crashes; no FPS gain) | 330-345      |
| bundle_perf_sleep_hook                                                   | 0\|1                              | Installs the Sleep/SleepEx detours once; there is no disarm.                                                                     | X                        | 347-365      |
| bundle_perf_affinity_all                                                 | -                                 | Sets affinity to the system mask. Upstream fix_amd_cpu_stuttering already restores the system mask (U:client_patches.cpp:58-77). | P-minor                  | 367-374      |

### engine_perf.cpp (19)

| Command                                                                                                                            | What it does                                                                                                                                                                   | Cat             | F line                    |
| ---------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------- | ------------------------- |
| bundle_perf_present_log [n=5]                                                                                                      | Logs the next n Present(sync_interval, flags) calls.                                                                                                                           | D               | 511-521                   |
| bundle_perf_present_interval auto\|0..4                                                                                            | Overrides sync_interval in the Present intercept.                                                                                                                              | P (setting)     | 523-543                   |
| bundle_perf_max_latency 0..16                                                                                                      | Calls IDXGIDevice1::SetMaximumFrameLatency once on the next Present.                                                                                                           | P (setting)     | 545-555, 433-448          |
| bundle_perf_present_stats                                                                                                          | Prints counters.                                                                                                                                                               | D               | 808-820                   |
| bundle_perf_present_stack                                                                                                          | Prints a one-time 8-frame Present call stack.                                                                                                                                  | D (renderer RE) | 739-760, 479-489          |
| bundle_perf_draw_hook                                                                                                              | Detours ID3D11DeviceContext vtable entries 12 DrawIndexed, 13 Draw, 14 Map, 15 Unmap, 20 DrawIndexedInstanced and 21 DrawInstanced. The vtable comes from a dummy WARP device. | D (profiler)    | 557-559, 297-361          |
| bundle_perf_draw_stats / bundle_perf_draw_callers [n] / bundle_perf_draw_l2 / bundle_perf_draw_callers_l2 / bundle_perf_draw_reset | Draw counts plus L1/L2 caller histograms. L2 uses RtlCaptureStackBackTrace with skip 3 and costs about 4% while on. The histogram is a lock-free 512-slot table.               | D               | 561-618, 640-679, 719-737 |
| bundle_perf_draw_skip_add hexRA / bundle_perf_draw_skip_clear / bundle_perf_draw_skip_list                                         | Drops draws issued by up to 16 return addresses.                                                                                                                               | X               | 681-717                   |
| bundle_perf_rsl_hook                                                                                                               | Detours base+0x1C53FD0 (RenderSurfaceList).                                                                                                                                    | X               | 620-622, 273-295          |
| bundle_perf_render_skip 1..100                                                                                                     | Divides SurfaceList.count (at +0x10) by the factor, then restores it.                                                                                                          | X               | 624-638, 253-271          |
| bundle_perf_wait_hook / bundle_perf_wait_stats / bundle_perf_wait_reset                                                            | Detours WaitForSingleObjectEx and WaitForMultipleObjectsEx; accounts only waits on the render thread.                                                                          | X               | 762-806, 377-431          |

## 3. Hooks and patches active on every launch (not commands)

| Installed in                       | Target (old exe)                                                                            | Mechanism                                                                          | Effect when idle                                                                                             | F line               |
| ---------------------------------- | ------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ | -------------------- |
| workshop post_unpack               | base+0x1425169                                                                              | 1-byte write 0x75 to 0xEB. Refuses if the byte is not 0x75; no-op if already 0xEB. | Always patched.                                                                                              | 1899-1931, 1936      |
| workshop                           | DB_UnloadXZone 0x141425A70 (prologue `48 89 5C 24 08`)                                      | detour                                                                             | Skips slots 15/16 when g_skip_unload is on.                                                                  | 1557-1617, 1939      |
| workshop                           | unload_ff_mapping 0x14141F040, called from LOOP2 at 0x141424BF6 with rcx=&g_zoneNames[slot] | detour                                                                             | Skips core_mod and en_core_mod when g_skip_unload is on.                                                     | 1619-1644, 1945      |
| workshop                           | 0x142179960, the 'descriptor allocator' called from the 0x1413F0E80 wrapper                 | detour                                                                             | Trace only.                                                                                                  | 1575-1599, 1951      |
| workshop                           | command-file watcher thread                                                                 | std::thread                                                                        | Polls every 300 ms.                                                                                          | 1646-1829, 1955-1956 |
| workshop                           | kernel32!CreateFileW                                                                        | detour                                                                             | Trace when the flag is on. The CreateFileA detour crashed at boot.                                           | 1831-1888, 1964-1968 |
| workshop                           | kernel32!CreateThread, ntdll!NtCreateThreadEx                                               | detours                                                                            | Always forces CREATE_SUSPENDED, optionally sets DR0, then resumes.                                           | 289-380, 3784-3793   |
| workshop                           | call at 0x141422AF2 to Com_Printf                                                           | call-site hook                                                                     | Captures SPT names during bundle_load_ff, then forwards to the hardcoded 0x142148F60 with exactly 4 varargs. | 62-109, 3800         |
| workshop                           | 0x1412C8020 ('Scr_EndLoadScripts' wrapper)                                                  | detour                                                                             | Prints every call together with the animtree flag.                                                           | 114-127, 3805-3806   |
| workshop                           | Scr_BeginLoadScripts 0x1412C7DF0                                                            | detour                                                                             | Prints enter and exit; arms DR0 while the hunt is on.                                                        | 456-495, 3812-3813   |
| workshop                           | Scr_LoadScript 0x1412C83F0                                                                  | detour                                                                             | Prints every call, keeps a return-address histogram, and sets DR0 per call while the hunt is on.             | 497-559, 3819        |
| patches                            | Com_Error_ 0x1420F8170                                                                      | existing upstream detour, plus 2 suppressions                                      | PMem suppression is always active.                                                                           | 285-308, 355         |
| client_patches                     | 0x1422A2AF0 and 0x1422A28D0                                                                 | detours                                                                            | Print every call with the caller address.                                                                    | 133-164, 228-233     |
| script.cpp begin_load_scripts_stub | -                                                                                           | direct call                                                                        | Drains captured names when enabled.                                                                          | 601-618              |
| download_overlay present_stub      | Present                                                                                     | in-stub call                                                                       | engine_perf::intercept_present runs every frame.                                                             | 352-357, 407         |
| console_logger                     | console interceptor                                                                         | set_interceptor                                                                    | Writes console.log, truncated on each launch.                                                                | 54-75                |
| updater                            | update()                                                                                    | early `return;`                                                                    | Self-update disabled.                                                                                        | updater.cpp:15       |

## 4. Exact behavior of the patches.cpp Com_Error suppression

**Hook**

- `com_error_hook.create(game::Com_Error_, com_error_stub)` at
  F:patches.cpp:355. It is a MinHook detour on Com_Error_ at old 0x1420F8170
  (fork symbols.hpp:43-44); the new address is 0x1420EB9F0
  (U:symbols/com/core.hpp:30-32).
- The detour existed upstream already. The PoC added the two branches below.

**What the stub does**

1. Formats the message with `vsnprintf_s` into `char buffer[0x1000]` (L108-115).
2. Runs the existing handling first:
   - server restart early-out (L117-120)
   - Lua cascade suppression (L122-126)
   - script and link error formatting (L128-268)
   - 'Clientfield Mismatch' converted to ERR_DROP (L274-283)
3. Then checks the two new conditions:
   - **'Could not load default asset'** (L289-294): suppressed only when
     `workshop::is_skip_unload_active()` (F:workshop.cpp:3844-3846) and
     `strstr(buffer, "Could not load default asset")`. The stub prints
     `[Com_Error] bo3-bundle: suppressing 'default asset' ...` and returns.
   - **PMem LIFO** (L303-308): suppressed when
     `strstr(buffer, "PMem_Free") && strstr(buffer, "not at the top of the stack")`.
     This branch is **not gated**. The stub prints and returns.
4. Only when neither condition matches does it continue to the ERR_DROP popup
   (L310-317) and call the original (L320).

**Effect**

- On a match, the original Com_Error never runs, so the engine does no longjmp
  or abort.
- Execution resumes after the normally no-return Com_Error call inside the
  engine. For PMem LIFO, that point is in PMem_Free (old 0x141439190, new
  0x1420C48B0).
- Correctness depends on whatever code follows that call. The error code is
  ignored.
- The PoC's rationale (comment L296-302): Solo's core_mod at slot 16 and DS4C's
  core_mod at slot 23 (both named "core_mod") break the PMem stack order on a
  map change. Suppressing the error leaks the original allocation.
- The committed code cannot produce slot 23, because the redirect was never
  implemented.

**Upstream today**

- U:patches.cpp:143-365 has neither suppression.
- It logs the derelocated caller (L145, 325-328), so any future suppression can
  be keyed to a caller RVA instead of message text.

## 5. Findings: claim, evidence, old addresses, signature, verification

"Dump" means a static check on a live-process memory dump: the game must be
running, but nobody has to play.

**F1. loadMod is not additive** (e5968d0, RT)

- Claim: with reloadFS=false, each call still runs ShutdownGame and FS_Startup
  and keeps only the new mod's path.
- Addresses: old 0x1420D6930, which is UGC_LoadModByPublisherId at new
  0x1420CA1B0.
- Upstream: it now re-implements this as `UGC_LoadModByPublisherId_Impl`, which
  calls `UGC_LoadMod` (U:game/impl/ugc/ugc.cpp:276-323).
- Re-verify: live game.

**F2. FS search-path insertion** (ee8bcb3/9071412)

- Claims:
  - Calling 0x1422A28D0(path, gamedir, 0, 0) inserts a node while a mod is
    active, without a crash. RT.
  - New nodes are always appended. RT.
  - f1=1 takes a localization format branch; f2 has no effect on position. RT.
  - The list printed by the dump matches the game's own "Current search path:"
    output. RT.
- Head pointer: old 0x157A65308. It comes from `mov reg,[rip+disp32]` at
  0x1422A293B: a 7-byte instruction, disp32 0x157C29C6, next RIP 0x1422A2942.
  That instruction is FS_AddGameDirectory+0x6B. DIS.
- Node layout: `{next@0, data@8}`, with `data->path` at +0 and `data->gamedir`
  at +0x100.
- Upstream mapping:
  - 0x1422A28D0 is
    `FS_AddGameDirectory(path, dir, bLanguageDirectory, iLanguage)`, new
    0x142245DB0. This explains the f1 observation.
  - The head sits between fs_game (0x157A652F8) and fs_loadStack (0x157A65310).
    Both shift by -0x7EF00, giving a candidate new head of 0x1579E6408.
- Signature: in FS_AddGameDirectory (new 0x142245DB0), find the qword
  RIP-relative load near +0x6B (`48/4C 8B modrm(rm=101) disp32`) whose target
  equals fs_loadStack-8.
- Re-verify: the address on a dump; the behavior in a live game.

**F3. Search-path promotion** (9071412/88912f5, RT)

- Claim: moving the tail node to the head works, and SoE cheats still fire
  afterwards.
- No locking. The comment at F:3360-3363 calls this "safe in practice".

**F4. Direct DB_LoadXAssets and the zone tier table** (9071412/88912f5)

- Calling works, but detouring the prologue failed to boot
  (F:client_patches.cpp:235-239; workshop.cpp:2485-2490). RT.
- 22 callers were found by an xref scan. DIS.
- allocFlags 0x4 unloaded core_common, core_ui and core_post_gfx, then crashed
  with "Default sound assets not found". RT.
- Load call sites seen (DIS):
  - 0x142148732 uses allocFlags 0x800. Other values seen: 0x200, 0x800000,
    0x400000.
  - The mod-load site 0x1420F938F uses freeFlags 0x140.
- Tier table (RT, from the zone dump):

  | Zone              | Flags                                |
  | ----------------- | ------------------------------------ |
  | core_pre_gfx      | 0x1                                  |
  | core_gfx          | 0x2                                  |
  | core_post_gfx     | 0x4                                  |
  | core_ui           | 0x8                                  |
  | core_common       | 0x20                                 |
  | core_mod          | 0x40                                 |
  | `core_ffotd_*`    | 0x2000                               |
  | core_specialists  | 0x400000                             |
  | core_frontend     | 0x800000                             |
  | patch overlay bit | 0x10000000 (core_patch = 0x10000002) |
  | language-pair bit | 0x8000000 (en_core_mod = 0x8000040)  |

- Slots seen: 15 = en_core_mod, 16 = core_mod; frontend at 17, 18, 20 and 21
  (F:1603-1606).
- Corroboration: MS:mod_stack.cpp:28-29 has CORE_MOD_FLAGS=0x40 and
  MODE_MOD_FLAGS=0x100. So 0x140 is core tier plus mode tier, not "0x100 \|
  allocFlags" (INF).
- Re-verify: live game.

**F5. Hot-swap deliverable** (88912f5, RT)

- Claim: addpath, promote_last, then `bundle_load_ff core_mod 40` (alloc 0x40,
  free 0x140) replaced tier 0x40 with mod B's zones, with no crash and no popup.
  The same worked for zm_mod, cp_mod and mp_mod.
- This is not additive. The default freeFlags also free the mode-mod tier
  (F:2625).

**F6. `en_` auto-prefix wall** (88912f5, RT)

- Claim: loading into tier 0x80 (or 0x8000080) gives "Could not find zone
  'en_X'". Passing en_zm_mod produces en_en_zm_mod, so the prefix is based on
  the name and applied unconditionally.
- Upstream: DB_LoadXAssets expands the zone list through DB_ExpandZoneList (MS
  symbol, new 0x141426970, called at DB_LoadXAssets+0x347). Mod-stack keeps the
  same tier flags, aliases the names, and routes the files. INF: the wall was
  caused by naming and path resolution, not tier registration.

**F7. Validator patch** (d34d95d, RT + DIS)

- Claims: the 'ERROR: Could not find zone' Com_Error is guarded by a jnz at
  0x141425169. The validator call site is 0x14142515D. Both were found with
  bundle_findstrxref and patched with bundle_patch.
- The fork applies the patch unconditionally.
- Candidate new address: 0x141425189 (+0x20).
- Signature: the string "Could not find zone", its LEA xref, and the Jcc
  (`75 xx`) that guards the Com_Error call.
- Re-verify: the location on a dump; the effect in a live game.

**F8. Skip-unload** (d34d95d, RT)

- What was tried (outcomes from code comments):
  - NOPping ShutdownGame broke loadMod's state machine.
  - Skipping the frontend slots crashed.
  - The working version skips only slots 15/16 in DB_UnloadXZone and only
    core_mod/en_core_mod in unload_ff_mapping.
- No successful outcome was recorded, so this is X.
- Candidate new addresses: 0x14141F060 for unload_ff_mapping; 0x141424C16 for
  the LOOP2 site (DB_ReleaseXAssets+0x436 if it is the same function).

**F9. Slot/tier redirection** (d34d95d)

- Never implemented: the stub logs only (VER).
- 0x142179960 sits between Stream_CloseFile and Stream_OpenFile
  (U:symbols/stream.hpp:9-12), so it is a stream file-layer function, not a zone
  allocator (INF).
- Candidate new address: 0x142120EC0.

**F10. PMem LIFO** (d34d95d, RT)

- Claim: see section 4.
- The coexistence state behind it (slot 23) cannot be reproduced from the
  committed code.
- Design goal: avoid it by construction. Mod-stack uses unique aliased names
  inside the same DB_LoadXAssets batch.

**F11. Redundant-asset capture** (d34d95d)

- Call site 0x141422AF2 replaces a Com_Printf call. The PoC names Com_Printf
  (0x142148F60) 'Com_PrintWarning'.
- Varargs (DIS):
  - r9: the asset type name
  - [rsp+20]: the asset name
  - [rsp+28] and [rsp+30]: zone names
- Claim (RT): the second asset with the same name is still linked by head
  insertion "at 0x149877B70", and DB_FindXAssetHeader returns the newer one.
- Upstream:
  - The call site is DB_LinkXAssetEntry(0x1414228C0)+0x232; candidate new site
    0x141422B12. VER location.
  - 0x149877B70 is a slot inside g_assetEntryPool. The chain is
    XAssetEntry.nextOverride (U:structs/db/xasset/core.hpp:968-977). INF.
  - Mod-stack hooks DB_LinkXAssetEntry and DB_OverrideAsset (new 0x141423D20)
    for ownership and priority.
- Signature: the "Redundant ... asset" format-string xref inside
  DB_LinkXAssetEntry, followed by `E8 rel32` to Com_Printf (new 0x1420F04A0).

**F12. Activating a second mod's GSC** (d34d95d, RT, X)

- Activating every SPT asset, or draining captured names at begin_load_scripts,
  needed a cross-map filter. For example, zm_cosmodrome_ffotd hard-imports and
  caused "Error linking script".
- Extracting with rename (Path G phase 1a) does not patch imports.

**F13. "custom_scripts autoexec never fires"** (d34d95d, RT)

- Claim: test_autoexec.gsc compiled, loaded and linked, but its body never
  executed. This was on the April base.
- Upstream since 0d4604b6 (2026-06-14) generates autoexec bootstraps into custom
  scripts (U:component/gsc/gsc_compiler.cpp:524-530, 740) and runs them via
  execute_loaded_script, which calls Scr_LoadScript between Scr_BeginLoadScripts
  and End (U:script.cpp:604-642, 1250-1266). INF: the claim is likely obsolete.
- Re-verify: live game.

**F14. Autoexec "queue", runner and queue-add** (d34d95d, 4df651e, e3fadd0,
3b60fc6, 7805319)

- PoC claims:
  - The queue head pointer is static 0x1432E6000.
  - Entries are 8 bytes: {u32 flags (mask 0x30000; 0x10000 = PRIVATE, 0x20000 =
    AUTOEXEC), u32 index}.
  - The index points into an "export table" at 0x1450DC2D0 with a 28-byte
    stride.
  - Contents are overwritten each cycle. RT.
- Functions as the PoC named them (DIS):
  - Runner 'Scr_RunExports' at 0x1412D7F10 takes ecx=flag. Its loop reads
    [rcx+8] and [rcx+0xC]; its internal head load is at 0x1412D7FB0 (+0xA0).
  - The 'dispatcher wrapper' 0x1412C7F20 calls the runner, then clears the flag
    at 0x1412C7FC9 (that RIP was captured by the HW breakpoint).
  - The 'Scr_EndLoadScripts wrapper' 0x1412C8020 checks the flag and calls the
    dispatcher.
- Head-load scan (3b60fc6, DIS on the dump): "21" load sites, in clusters at
  0x1412D738C-0x1412D7999 (12), 0x1412D7FB0 (1) and 0x1412D82C0-0x1412D8B9C (9).
  The clusters add up to 22.
- Queue-add (7805319, capstone): entry 0x1412D8810, which takes (rcx, edx, r8d,
  r9d). The body at 0x1412D8B9C is: `mov rax,[rip+disp]` / `lea rdx,[rax+rcx*8]`
  / `mov eax,[rax+rcx*8]` / `and eax,0x30000` / `bts edi,0x11` / `mov [r15],edi`
  (write at 0x1412D8BC9). The commit reports 16 direct callers, 4 of them in
  0x14233xxxx; the list was not recorded.
- Rejected first guesses: 0x1412D848F is one byte before upstream Scr_SetString
  (0x1412D8490), so it is padding. 0x1412D8234 was the other guess.
- Reinterpretation (likely): this is the script-string (SL) system.
  - The flag the dispatcher clears is exactly scrAnimPub_t.animtree_loading
    (VER).
  - The 'export table' is exactly gScrMemTreePub (VER).
  - The code sits in the `SL_*` cluster (VER).
  - The bits match the RefString user byte and CoD hash-status bits
    (U:structs/sl.hpp).
  - Reading [rcx+8] matches a string-hash scan that starts at index 1.
  - Under this reading, 0x1412C7F20 is likely an EndLoadAnimTrees-style routine
    and the runner a SL shutdown-by-user routine.
- Consequence: never call the 'queue-add' function.
- Falsification test on a new-exe dump: does SL_GetString (new 0x1412D7B40) or
  SL_GetLowercaseString (new 0x1412D7A30) call the candidate 0x1412D8830? Does
  0x1412C8040 call the candidate 0x1412D7F30 with ecx=1 or 2?
- Byte signature, derived from the mnemonics and not byte-verified:
  `48 8B 05 ?? ?? ?? ?? 48 8D 14 C8 8B 04 C8 25 00 00 03 00 ... 0F BA EF 11 ... 41 89 3F`.

**F15. Hardware-breakpoint infrastructure** (d34d95d/4df651e/e3fadd0)

- What it does:
  - DR0/DR7 write traps of 1, 2, 4 or 8 bytes (L212-270).
  - A one-shot VEH handler that clears DR0 and sets RF (L174-207).
  - Arming on all threads via Toolhelp32, SuspendThread and Get/SetThreadContext
    (L388-454).
  - Re-arming per Scr_LoadScript call, plus hooks on thread creation.
- Results (RT): the flag breakpoint fired once. The queue breakpoint never
  fired, even with 123 threads armed.
- **F**: debug registers and thread-creation hooks belong to the anti-tamper
  layer upstream. utils/hardware_breakpoint's only user is arxan.cpp, and the
  PoC's own comment cites arxan.cpp's create_thread_hook (F:289-296, 3779-3783).

**F16. File-watcher command channel** (d34d95d/bfca64a, VER code)

- Mechanism:
  - Polls `%LOCALAPPDATA%\boiii\bundle_commands.txt` every 300 ms.
  - Truncates the file after reading.
  - Sends each non-# line through scheduler::once(main) to Cbuf_AddText.
  - Writes a breadcrumb file instead of printing, because a printf from that
    thread at startup crashed.
- Risk: there is no authentication, and bundle_patch is reachable through it.

**F17. FPS limiter patch** (bfca64a, DIS + RT)

- Location: Com_Frame_Try_Block_Function 0x1420F8E00; loop at 0x1420F8EE5; `jge`
  at 0x1420F8F0E, patched from 0x7D to 0xEB.
- Instruction sequence from the commit:
  `mov eax,0x3e8; idiv ebx; mov r12d,eax; L: call get_time; ebx=esi-earliest; cmp ebx,r12d; jge EXIT; call throttle; inc r14d; cmp r14d,0x32; jl L`.
  Here ebx = max(com_maxfps, secondary). The secondary cap is the function the
  PoC called 0x1407CFD0, which is really 0x140F7CFD0 and returns about 200.
- Superseded by upstream's Com_FPSLimit hook. The PoC patch additionally ignores
  com_maxfps.
- If it is ever needed again:
  - Candidate address: 0x1420EC78E, valid only if the function body is
    unchanged.
  - Byte signature (derived): setup `B8 E8 03 00 00 99 F7 FB 44 8B E0`; loop
    tail `41 FF C6 41 83 FE 32 7C ??`; the patch byte is the `7D` of the jge
    before the throttle `E8`.

**F18. Present hook sharing** (bfca64a, RT)

- MinHook refuses a second hook on vtable[8]. Upstream's `detour::create` throws
  in that case (U:src/common/utils/hook.cpp:243-244).
- So engine_perf::intercept_present is called from download_overlay's
  present_stub (F:download_overlay.cpp:357). It runs before ImGui and can
  rewrite sync_interval and flags by reference.
- Forcing sync=0 made no FPS difference.
- Becomes a P: a central Present dispatcher.

**F19. Renderer hot path** (00127ff/fd61f80/9ecb1bd; RT + DIS)

- Sample: about 22M draws over about 9k frames in zombies.
- L1 call sites: +0x1D0045E (22M) and +0x1D004F2 (1.3M), inside the
  DrawIndexedInstanced wrapper +0x1D00425 (about 30 instructions).
- L2 call sites: +0x1C54506 (7.5M, 91%), +0x1C772D1 (617K) and +0x1CD62FD
  (147K).
- `RenderSurfaceRange(rcx renderer, rdx SurfaceList{+8 surfaces**, +0x10 count}, r8 start)`
  is at +0x1C54350; its loop body is 0x141C543A0.
- The loop switches on draw_type to helpers 0:+0x1CDF520, 1:+0x1C54C60,
  2:+0x1C54AA0 and 3:+0x1D00425. The outer function RenderSurfaceList is
  +0x1C53FD0.
- Null results:
  - Cutting draws/frame from 2602 to 1467 and then 899 moved FPS from about 205
    to 213 and 214.
  - In the frontend: 299 Maps/frame against 666 Draws/frame.
- Byte signature (derived): `49 8B 5D 08 4A 8B 1C F3`
  (`mov rbx,[r13+8]; mov rbx,[rbx+r14*8]`) and `41 FF C6 45 3B 75 10 72 ??` (inc
  r14d; cmp r14d,[r13+0x10]; jb).
- Re-verify: dump.

**F20. Perf component** (bafcb8a/2211a93/2ea1140, RT)

- Applying HIGH priority, the 0.5 ms timer and the NtDelayExecution hook in
  post_load (bafcb8a:perf.cpp:222-230) crashed at "Loading fastfile
  'core_patch'". The same settings were safe when applied at runtime: 243 FPS on
  the attract screen, cap 250.
- Every Sleep/SleepEx variant crashed during transitions, including logging-only
  hooks during zm_patch load.
- Results: 60 FPS with vsync; about 200 FPS with vsync off, priority and timer.

**F21. Console logger** (ee8bcb3)

- Truncates `%LOCALAPPDATA%\boiii\console.log` per launch and timestamps each
  line (F:console_logger.cpp:28-68).
- It uses the single-slot `console::set_interceptor`. rcon's
  `scoped_interceptor` (U:rcon.cpp:42) clears that slot
  (U:console.cpp:1452-1458).

**F22. FS and DB tracing** (ee8bcb3/9071412)

- Detours on 0x1422A2AF0 (upstream FS_AddLocalizedGameDirectory) and 0x1422A28D0
  (upstream FS_AddGameDirectory), plus `[fs-trace]` prints in the players-folder
  and mods-path stubs (F:client_patches.cpp:103-131; workshop.cpp:888-917).
- The DB_LoadXAssets trace was removed (F:client_patches.cpp:166-186 is unused;
  235-239).

**F23. Updater bypass** (e5968d0)

- `return;` at F:updater.cpp:15. X.

## 6. Exact old-to-new map (upstream legacy column equals the PoC address)

| PoC name                                                   | Old                         | Upstream symbol (U: line)                                                                      | New                        | Note                                                                          |
| ---------------------------------------------------------- | --------------------------- | ---------------------------------------------------------------------------------------------- | -------------------------- | ----------------------------------------------------------------------------- |
| DB_LoadXAssets                                             | 0x1414236A0                 | db::load::DB_LoadXAssets (symbols/db/load.hpp:18-20)                                           | 0x1414236C0                | Use call-site hooks, not the prologue                                         |
| DB_UnloadXZone                                             | 0x141425A70                 | db::load::DB_UnloadXZone (load.hpp:29-30)                                                      | 0x141425A90                |                                                                               |
| DB_LinkXAssetEntry (contains 0x141422AF2)                  | 0x1414228C0                 | load.hpp:21-23                                                                                 | 0x1414228E0                |                                                                               |
| DB_ReleaseXAssets (probably contains 0x141424BF6)          | 0x1414247C0                 | load.hpp:31                                                                                    | 0x1414247E0                | host is INF                                                                   |
| DB_EnumXAssets                                             | 0x141420970                 | db::xasset (symbols/db/xasset/core.hpp:12)                                                     | 0x141420990                |                                                                               |
| DB_FindXAssetHeader                                        | 0x141420ED0                 | xasset/core.hpp:15                                                                             | 0x141420EF0                | upstream detours it for overrides                                             |
| DB_FileExists(zone, src)                                   | 0x141420B40                 | db::DB_ValidFastFile (db/core.hpp:13-14)                                                       | 0x141420B60                |                                                                               |
| g_zoneCount                                                | 0x14941097C                 | db::xzone (db/xzone.hpp:12)                                                                    | 0x14939197C                |                                                                               |
| g_zoneNames[64] (0x60 each)                                | 0x14998FB80                 | db/xzone.hpp:14-15                                                                             | 0x149910B80                |                                                                               |
| g_zoneInfo / g_zoneInfoCount (related)                     | 0x149995020 / 0x149996620   | db/xzone.hpp:18-20                                                                             | 0x149916020 / 0x149917620  | XZoneInfoInternal list with alloc/free flags                                  |
| g_assetEntryPool (contains 0x149877B70)                    | 0x1494C7B80                 | db/xasset/pool.hpp:16-17                                                                       | 0x149448B80                |                                                                               |
| loadMod                                                    | 0x1420D6930                 | ug<local scratch copy, not kept>:UGC_LoadModByPublisherId (symbols/ugc.hpp:23-25)              | 0x1420CA1B0                |                                                                               |
| inner_loadMod                                              | 0x1420D6B50                 | ug<local scratch copy, not kept>:UGC_LoadMod (ugc.hpp:20-22)                                   | 0x1420CA3D0                |                                                                               |
| isModLoading                                               | 0x1420D6DE0                 | ug<local scratch copy, not kept>:UGC_ModLoad_TryComplete (ugc.hpp:26-27)                       | 0x1420CA660                | different name                                                                |
| isModLoaded                                                | 0x1420D5020                 | ug<local scratch copy, not kept>:UGC_ActiveMod_Loaded (ugc.hpp:17)                             | 0x1420C88A0                |                                                                               |
| getPublisherIdFromLoadedMod                                | 0x1420D7680                 | ug<local scratch copy, not kept>:UGC_ActiveMod_PublisherId (ugc.hpp:18-19)                     | 0x1420CAF00                |                                                                               |
| reloadUserContent                                          | 0x1420D66C0                 | ug<local scratch copy, not kept>:UGC_LoadPools (ugc.hpp:49)                                    | 0x1420C9F40                |                                                                               |
| active mod (commit says ~0x14878CCA0)                      | 0x15678CCA0                 | ug<local scratch copy, not kept>:active_mod, ActiveMod 0x4D0 (ugc.hpp:30)                      | 0x15670DD20                | PoC typo                                                                      |
| modsCount / modsPool (stride 0x4C8)                        | 0x15678D170 / +8            | ug<local scratch copy, not kept>:builtinModsPool (ugc.hpp:9-10)                                | 0x15670E1F0 / +8           | upstream also keeps its own 8192-slot pool                                    |
| usermapsCount / usermapsPool                               | 0x1567B3580 / +8            | ug<local scratch copy, not kept>:builtinUsermapsPool (ugc.hpp:13-14)                           | 0x156734600 / +8           |                                                                               |
| usermap_title / internal_usermap_id / usermap_publisher_id | 0x1567D99A0 / +0x64 / +0x84 | ug<local scratch copy, not kept>:active_usermap (ugc.hpp:29)                                   | 0x15675AA20 (same offsets) |                                                                               |
| Scr_LoadScript                                             | 0x1412C83F0                 | scr (symbols/scr/core.hpp:79-80)                                                               | 0x1412C8410                |                                                                               |
| Scr_BeginLoadScripts                                       | 0x1412C7DF0                 | scr/core.hpp:81-82                                                                             | 0x1412C7E10                |                                                                               |
| begin_load_scripts call site (upstream hook)               | 0x141AAE92F                 | component/script.cpp:1489-1490                                                                 | 0x141AA255F                |                                                                               |
| flag `base+0x4D4E5CC+inst*0x820`                           | 0x144D4E5CC                 | gScrAnimPub (scr/vm/anim.hpp:9-10) + animtree_loading at 0x81C (structs/scr/vm/anim.hpp:12-23) | 0x144CCF5CC                |                                                                               |
| 'export table base'                                        | 0x1450DC2D0                 | scr::mt::gScrMemTreePub (scr/mt.hpp:17-18)                                                     | 0x14505D2D0                |                                                                               |
| Com_Error_                                                 | 0x1420F8170                 | com (symbols/com/core.hpp:30-32)                                                               | 0x1420EB9F0                |                                                                               |
| 'Com_PrintWarning'                                         | 0x142148F60                 | com::Com_Printf (com/core.hpp:27-29)                                                           | 0x1420F04A0                |                                                                               |
| Com_IsInGame                                               | 0x1421482C0                 | com/core.hpp:49                                                                                | 0x1420EF860                |                                                                               |
| Com_LoadLevelFastFiles (probably contains 0x142148732)     | 0x1421484E0                 | com/core.hpp:53-54                                                                             | 0x1420EFA80                |                                                                               |
| Com_Frame_Try_Block_Function                               | 0x1420F8E00                 | com/core.hpp:76-77                                                                             | 0x1420EC680                | upstream detours it (scheduler.cpp:274-275)                                   |
| hidden cap ("0x1407CFD0")                                  | 0x140F7CFD0                 | com::Com_FPSLimit (com/core.hpp:86-90)                                                         | 0x140F7CFD0                | upstream hook at client_patches.cpp:564-567                                   |
| Cbuf_AddText                                               | 0x1420EC010                 | cbuf.hpp:8                                                                                     | 0x1420DF890                |                                                                               |
| CL_SetupForNewServerMap                                    | 0x14135CD20                 | cl.hpp:22                                                                                      | 0x14135CD40                |                                                                               |
| UI_OpenErrorPopupWithMessage                               | 0x14228DEE0                 | ui/core.hpp:11                                                                                 | 0x1422313C0                |                                                                               |
| 'FS_AddSearchPath'                                         | 0x1422A28D0                 | fs::FS_AddGameDirectory(path, dir, bLanguageDirectory, iLanguage) (symbols/fs.hpp:35-37)       | 0x142245DB0                | different name                                                                |
| 'FS_AddGameDirectory'                                      | 0x1422A2AF0                 | fs::FS_AddLocalizedGameDirectory (fs.hpp:38-39)                                                | 0x142245FD0                | different name                                                                |
| 'function_4400'                                            | 0x1422A4400                 | fs::FS_Startup (fs.hpp:40-41)                                                                  | 0x1422478E0                |                                                                               |
| FS_FOpenFileWriteToDir / ReadFromDir                       | 0x1422A35D0 / 0x1422A3510   | fs.hpp:11-16                                                                                   | 0x142246AB0 / 0x1422469F0  |                                                                               |
| fs_game ptr / fs_loadStack (neighbors of the list head)    | 0x157A652F8 / 0x157A65310   | dvar/core.hpp:49; fs.hpp:7                                                                     | 0x1579E63F8 / 0x1579E6410  |                                                                               |
| PMem_Free                                                  | 0x141439190                 | pmem::PMem_Free (symbols/pmem.hpp:11-12)                                                       | 0x1420C48B0                | The legacy value is far from the other PMem functions (0x1420D0F40); re-check |

## 7. PoC addresses with no upstream symbol (must be re-found)

Candidate new addresses are old address plus the shift of the nearest upstream
anchors, computed by the scratchpad `anchor_deltas.py`. All candidates are INF
until a dump confirms them.

| PoC item                                   | Old                                                                                                                                               | Anchors (shift)                                    | Candidate new                           | Confidence      | How to re-find                                           |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | --------------------------------------- | --------------- | -------------------------------------------------------- |
| search-path list head                      | 0x157A65308                                                                                                                                       | fs_game, fs_loadStack (-0x7EF00)                   | 0x1579E6408                             | high            | RIP-relative operand in FS_AddGameDirectory              |
| RIP-relative load of the head              | 0x1422A293B                                                                                                                                       | FS_AddGameDirectory+0x6B (-0x5CB20)                | 0x142245E1B                             | medium          |                                                          |
| 'FS_ShutdownSearchPaths'                   | 0x1422A4280                                                                                                                                       | FS_JoinPath / FS_Startup (-0x5CB20)                | 0x142247760                             | medium          | function just before FS_Startup                          |
| unload_ff_mapping                          | 0x14141F040                                                                                                                                       | `DB_*` (+0x20)                                     | 0x14141F060                             | medium          | callee at the LOOP2 site                                 |
| LOOP2 call site                            | 0x141424BF6                                                                                                                                       | DB_ReleaseXAssets+0x436                            | 0x141424C16                             | medium          | rcx=&g_zoneNames[slot]; "Unloaded fastfile '%s'" follows |
| validator call / jnz                       | 0x14142515D / 0x141425169                                                                                                                         | +0x20                                              | 0x14142517D / 0x141425189               | medium          | "Could not find zone" xref                               |
| lookup wrapper                             | 0x1413F0E80                                                                                                                                       | +0x20 (anchors far)                                | 0x1413F0EA0                             | low             |                                                          |
| 'descriptor allocator'                     | 0x142179960                                                                                                                                       | Stream_CloseFile / Stream_OpenFile (-0x58AA0)      | 0x142120EC0                             | medium-high     | between new 0x142120D70 and 0x142122480                  |
| 'Redundant asset' call                     | 0x141422AF2                                                                                                                                       | DB_LinkXAssetEntry+0x232                           | 0x141422B12                             | medium-high     | string xref, then E8 to Com_Printf                       |
| load-zone call (alloc 0x800)               | 0x142148732                                                                                                                                       | Com_LoadLevelFastFiles+0x252 (-0x58A60 / -0x58AC0) | 0x1420EFCD2                             | low             | E8 to DB_LoadXAssets                                     |
| mod-load call (free 0x140)                 | 0x1420F938F                                                                                                                                       | -0xC780 below, anchor far above                    | 0x1420ECC0F                             | low             | E8 to DB_LoadXAssets with 0x40/0x140                     |
| 'Scr_EndLoadScripts' wrapper               | 0x1412C8020                                                                                                                                       | +0x20                                              | 0x1412C8040                             | medium          | reads 0x144CCF5CC                                        |
| 'dispatcher' / flag clear                  | 0x1412C7F20 / 0x1412C7FC9                                                                                                                         | +0x20                                              | 0x1412C7F40 / 0x1412C7FE9               | medium          | write to animtree_loading                                |
| function-lookup helper                     | 0x1412CC0F0                                                                                                                                       | +0x20                                              | 0x1412CC110                             | medium          | sets GSC_EXPORT_ITEM.flags (+0x11) bit 0x1               |
| runner / its internal load                 | 0x1412D7F10 / 0x1412D7FB0                                                                                                                         | SL cluster (+0x20)                                 | 0x1412D7F30 / 0x1412D7FD0               | medium          | see F14                                                  |
| 'queue-add' / its write                    | 0x1412D8810 / 0x1412D8BC9 (load at 0x1412D8B9C)                                                                                                   | +0x20                                              | 0x1412D8830 / 0x1412D8BE9 (0x1412D8BBC) | medium          | see F14                                                  |
| 'queue head' pointer                       | 0x1432E6000                                                                                                                                       | builtin_methods / var_typename (-0x7F000)          | 0x143267000                             | high (location) | RIP-relative loads from the SL cluster                   |
| script loader wrapper / its call           | 0x140881BE0 / 0x140881C11                                                                                                                         | CG anchors (shift 0, far)                          | same                                    | low             | Scr_LoadScript caller histogram                          |
| 'SPT list head'                            | 0x149877B70                                                                                                                                       | inside g_assetEntryPool                            | n/a (not static)                        | -               | use DB_LinkXAssetEntry / nextOverride                    |
| FPS loop / jge                             | 0x1420F8EE5 / 0x1420F8F0E                                                                                                                         | Com_Frame+0xE5 / +0x10E                            | 0x1420EC765 / 0x1420EC78E               | low             | superseded                                               |
| RenderSurfaceList                          | 0x141C53FD0                                                                                                                                       | renderer (-0xC3D0; anchors 0x1EC20 / 0x63040 away) | 0x141C47C00                             | medium          |                                                          |
| RenderSurfaceRange / its body              | 0x141C54350 / 0x141C543A0                                                                                                                         | -0xC3D0                                            | 0x141C47F80 / 0x141C47FD0               | medium          | F19 byte signature                                       |
| L2 call sites                              | 0x141C54506 / 0x141C772D1 / 0x141CD62FD                                                                                                           | -0xC3D0                                            | 0x141C48136 / 0x141C6AF01 / 0x141CC9F2D | medium          |                                                          |
| draw-type helpers 0 / 1 / 2                | 0x141CDF520 / 0x141C54C60 / 0x141C54AA0                                                                                                           | -0xC3D0                                            | 0x141CD3150 / 0x141C48890 / 0x141C486D0 | medium          |                                                          |
| DrawIndexedInstanced wrapper and its calls | 0x141D00425 / 0x141D0045E / 0x141D004F2                                                                                                           | -0xC3D0                                            | 0x141CF4055 / 0x141CF408E / 0x141CF4122 | medium          | vtable[20] call                                          |
| "FastFileLoad: Setting" string             | about 0x142FC0C30 (derived from runtime 0x7ff77e7c0c30 with base 0x7FF77B800000, which DB_LoadXAssets' runtime 0x7ff77cc236a0 in 9071412 implies) | -                                                  | -                                       | low             | string scan                                              |

## 8. Where upstream already overlaps the PoC

**Working tree (5aa7fac)**

- **Mod loading.** `UGC_LoadModByPublisherId_Impl`
  (U:game/impl/ugc/ugc.cpp:276-323) synthesizes WorkshopData for missing mods
  and applies per-mod `assetlimits.txt` / `assetpools.txt` (docs/asset-pool.md)
  before calling UGC_LoadMod. The mods pool is extended to 8192 entries
  (U:structs/ugc.hpp:204-206).
- **Asset override.** RAWFILE and SCRIPTPARSETREE assets are overridden from
  `%LOCALAPPDATA%/boiii/data` and `<game>/boiii`, including mod-specific
  (publisherId) and map-specific subtrees (docs/asset-override.md). This works
  through a DB_FindXAssetHeader detour (U:script.cpp:1135-1167, 1482-1483).
- **Asset dumper.** `component/dump.cpp` dumps RAWFILE, SCRIPTPARSETREE and
  LOCALIZE_ENTRY assets (overlaps bundle_extract_ds4c).
- **Scripts.**
  - Trees: `load_tree("scripts")` overrides and
    `load_tree("custom_scripts", exec)` executes, both per mode, map and mod
    (U:script.cpp:733-893).
  - They run inside begin_load_scripts_stub, which is hooked at a call site
    (1250-1266, 1489).
  - The GSC compiler supports `detour` and `replacefunc`, each a single
    replacement with no chaining (docs/gsc-scripting.md:125-185).
- **FS.**
  - The FS_BuildOSPath hook (U:patches.cpp:504-518) and path helpers
    (U:component/path.cpp) exist.
  - FS_AddGameDirectory and FS_AddLocalizedGameDirectory are symbolized, but
    there is no fs_searchpaths symbol or searchpath struct (U:structs/fs.hpp is
    empty).
- **Perf.**
  - Com_FPSLimit returns UINT32_MAX (client_patches.cpp:564-567).
  - The game's own SetPriorityClass call is NOPed (464-465).
  - The AMD affinity fix sets 4 cores, then restores the system mask (58-77).
  - The scheduler detours Com_Frame (scheduler.cpp:274-275).
- **Present.** There is a single detour in download_overlay
  (U:download_overlay.cpp:404-448).
- **Tooling.** `utils::hook::signature` exists (src/common/utils/signature.hpp);
  its only consumer is arxan.cpp. The Com_Error stub logs the caller RVA.

**upstream/mod-stack** (c6676a31 + ac356c3c; 208 commits ahead of the working
tree; not in upstream/main)

- `fs_extramods` is a comma-separated list, highest priority first; the main mod
  (fs_game) always wins. It can also be set on the command line
  (MS:mod_stack.cpp:82-95, 780-795).
- `prepare_zones_stub` replaces the call at `DB_LoadXAssets+0x347` (new client;
  +0x32D on the server; 0x0 on legacy) (181-266):
  - For every mod zone, it adds `stack_<id>_<zone>` with the same allocFlags.
  - It adds core_mod (0x40) or `<mode>_mod` (0x100) when the main mod lacks
    them.
  - It then calls `DB_ExpandZoneList`.
- `schedule_zones_stub` replaces `+0x353` with `DB_LoadXZone` (268-270).
- The `FS_BuildOSPathForThread` detour routes aliased names to the real folder
  and file name (272-293).
- `KeyValuePairs_GetNextValue` rewrites `xpak_read` dependencies (295-330).
- `DB_LinkXAssetEntry` and `DB_OverrideAsset` are hooked for ownership and
  priority (625-650, 805-808).
- Sound-bank routing, precache merge (371-425) and UI ownership (426-445).
- Script pairing: the .gsc and .csc halves always come from the same mod. Entry
  scripts run as renamed copies `_clientids_<publisherId>.gsc` via
  `mod_stack::script_copies()` in `load_scripts()`.
- A Lua ModStack library is exposed to the menus. Server `getinfo` and lobbyHost
  advertise `modStack`, and joining players are prompted to download missing
  mods.
- It is disabled unless the build is the new client or the server (781-783).
- New symbols it adds (new / server):
  - `DB_OverrideAsset` 0x141423D20 / 0x1401D8DB0
  - `DB_ExpandZoneList` 0x141426970 / 0x1401DBA70
  - `DB_LoadXZone` 0x141423A70 / 0x1401D8AD0
  - `UGC_ModLoad_XZoneLoading` 0x1420F2B60
  - `KeyValuePairs_GetNextValue` 0x1420FBEE0
  - `UIElement_RenderLoadedMod` 0x1422302C0

## 9. `tools/*.py` and how to reuse them

| Script                         | What it does                                                                                                                                                                                                                                                                                                                                           | Limitations on the current exe                                                                                                                                                                                                                                                                 |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| dump_bo3_memory.py (190 lines) | Finds the boiii.exe process (falling back to BlackOps3.exe) and opens it with VM_READ \| QUERY_INFORMATION. Locates the BlackOps3.exe module and reads 0x1000 bytes of PE header. Reads each section with one page-rounded ReadProcessMemory and writes `<name>_va_0x<VA>.bin`, plus `pe_header.bin` and `manifest.txt` (runtime base and ASLR slide). | The process name is hardcoded. One read per section: `.data` is about 374 MB, and a single uncommitted page fails the whole read, so reads must be chunked with VirtualQueryEx. It also dumps the second `.text` and `.interpr` (fenced). Dumps contain live process data and must stay local. |
| scan_dump.py (144)             | Scans `.text`/`.interpr` dumps for RIP-relative references to a target (default 0x1432E6000). Uses REX.W `48/4C` opcodes `8B/8D/89` and `48 39/3B/03/09/0B/21/23/31/33` with rm=101 (assumes 7-byte length), plus `E8/E9` and `FF15/FF25`.                                                                                                             | Misses forms with immediates (`48 83 3D`, `81 3D`, `C7 05`), 32-bit `8B 05`, and `movzx` or `44 8B` forms. Also scans fenced sections.                                                                                                                                                         |
| find_queue_writer.py (169)     | In the first `.text`, finds `48 8B/8D` loads of the target, then looks for `C7 /0` imm32 stores on the same base register within the next 96 bytes.                                                                                                                                                                                                    | Cannot match the real write `41 89 3F` (INF: the 3b60fc6 result came from clustering plus manual disassembly).                                                                                                                                                                                 |
| disasm_func.py (73)            | capstone linear disassembly from a static address, up to 4096 bytes, stopping at ret. Requires `pip install capstone`.                                                                                                                                                                                                                                 | No control-flow graph or xrefs.                                                                                                                                                                                                                                                                |
| scan_xrefs.py (179)            | Same xref scan on the on-disk exe at a hardcoded Steam path.                                                                                                                                                                                                                                                                                           | Finds nothing on the encrypted client (3b60fc6). Possibly usable on unprotected binaries such as the dedicated server (unverified).                                                                                                                                                            |

**Re-verification pipeline for Xerces**

1. Run the game to the main menu with a Xerces dev build. No debugger and no HW
   breakpoints.
2. Dump with chunked reads: first `.text` (VA 0x1000-0x2EFD600), `.rdata` and
   `.data` only. Write outside the repo.
3. Offline checks:
   - Sanity-check a sample of upstream new-client addresses (prologues).
   - For each item in section 7, start at the candidate, disassemble with
     capstone, and match the sequences recorded in sections 5 and 7.
   - Derive data globals from RIP-relative operands inside anchored functions.
   - Generate signatures with disp32/rel32 bytes wildcarded, and require exactly
     one match in the first `.text`.
4. Emit JSON (name to RVA, signature, anchor, version). At runtime, resolve in
   post_unpack with `utils::hook::signature`, restricted to the first `.text`.
   Fail closed: disable the feature on zero or multiple matches.
5. Check behavior live: hot-swap, autoexec, PMem, FPS.

`scratchpad/anchor_deltas.py` re-derives the candidates in section 7 from the
upstream tables.

## 10. Inconsistencies in the PoC record (do not trust uncorrected)

**Address typos**

- `0x1407CFD0` (bfca64a, F:perf.cpp:223) is below the image base; the real
  function is 0x140F7CFD0.
- `~0x14878CCA0` (88912f5) is 0x15678CCA0.
- The comment at F:3394 gives head `0x157A6530F`; the code at 3404-3408 uses
  0x157A65308.
- 88912f5 gives "FS_Startup 0x1422A4280-0x1422A45F0" and separately
  "function_4400 0x1422A4400"; upstream FS_Startup is 0x1422A4400.

**Counts and labels**

- 3b60fc6 says "21 loads", but its clusters add to 22. Its entry guesses
  (0x1412D848F, 0x1412D8234) were superseded by 0x1412D8810.
- The commits describe queue entries as 8 bytes {flags@0, idx@4}, but the dump
  command defaults to stride 0x10 and reads flags@+8 and idx@+0xC (F:2980-2981,
  153-154).
- d34d95d calls 0x141422AF2 a "detour"; it is a call-site hook.
- engine_perf's message describes the wrapper as "the 15M-hits one" and then
  reports 22M.

**Stale code comments**

- The bundle_load_ff usage text says arg3 is sync; the code uses arg3 as
  freeFlags and arg4 as sync (F:2597-2598 vs 2626-2638).
- "File-open tracing is on globally from boot" (F:2658) contradicts
  `g_trace_file_opens{false}` (1843).

**Timeline**

- 2ea1140 removed the Sleep hooks; bfca64a re-added them, and its 210-300 FPS
  result includes "Sleep skip mid-game".

## Design implications

- Base the additive multi-mod loader on upstream mod-stack's architecture, not
  on the PoC's new-tier, hot-swap and suppression path. Mod-stack:
- loads extra zones under aliased names (stack_<id>_<zone>) in the existing
  tiers (0x40 core, 0x100 mode);
- hooks the DB_ExpandZoneList / DB_LoadXZone call sites inside DB_LoadXAssets;
- routes files via FS_BuildOSPathForThread;
- resolves priority via DB_OverrideAsset and tracks ownership via
  DB_LinkXAssetEntry. Rebase onto it once it lands, or port it with attribution.
- Asset ownership map and conflict detection:
- Record (type, name, zoneIndex) to mod in a DB_LinkXAssetEntry hook (new
  0x1414228E0).
- Apply manifest priority for exclusive assets in DB_OverrideAsset (new
  0x141423D20).
- Reuse upstream's DB_FindXAssetHeader override and the RAWFILE/SPT data trees
  as the injection point for merged stringtables.
- GSC hook registry: extend upstream's detour/replacefunc redirect backend,
  currently a single replacement, into chained callbacks with deterministic
  order. Keep mod-stack's rules that a script's .gsc and .csc come from the same
  mod and that entry scripts get renamed copies.
- Do not port:
- Com_Error suppressions, the validator patch, skip-unload, inject_zone and slot
  redirection;
- activate_scripts, auto-drain and extract_ds4c;
- the queue-add path;
- the FPS byte patch;
- Sleep, Wait, draw-skip and RSL experiments;
- the updater bypass.
- Production primitives to re-implement with signature-resolved addresses:
- read-only zone table view (g_zoneNames/g_zoneCount/XZoneName);
- SPT/asset inventory (DB_EnumXAssets);
- FS search-path diagnostics, with fs_searchpaths derived from
  FS_AddGameDirectory's RIP-relative operand (candidate new 0x1579E6408);
- FS_AddGameDirectory/FS_AddLocalizedGameDirectory wrappers under upstream's
  names and signatures;
- a DB_LoadXAssets batch wrapper used only for aliased same-tier loads;
- a central Present dispatcher;
- opt-in perf settings (timer resolution, priority, max frame latency, sync
  interval) applied after the main menu and reverted on exit;
- a console log tee on a multi-subscriber interceptor.
- Put the RE tools in a dev-only module that is compiled out of release builds:
  disasm, dumpbytes, findstring, findxref, findstrxref, finddataref,
  zone/searchpath/script dumps, the Scr_LoadScript caller histogram, the
  draw/present profilers and the file-open trace. Never ship bundle_patch, HW
  breakpoints, thread hooks, SendInput automation or the file watcher. If a
  remote command channel is needed, use an authenticated named pipe behind a dev
  flag.
- Offset policy:
- Treat upstream's new-client column as ground truth for the roughly 3000
  symbols it covers.
- For gaps, build signatures offline from a live-process dump.
- Resolve at post_unpack with utils::hook::signature, scanning only the first
  .text (VA 0x1000-0x2EFD600).
- Derive data globals from RIP-relative operands of anchored functions.
- Fail closed on zero or multiple matches.
- Never write bytes without first verifying the whole expected instruction
  sequence.
- Add a hook bus that multiplexes detours on shared targets (Present,
  Com_Error_, DB_LinkXAssetEntry, DB_OverrideAsset, FS_BuildOSPathForThread,
  Scr_BeginLoadScripts call site), because MinHook / utils::hook::detour cannot
  hook a target twice.
- Lifecycle rules from the PoC's crashes:
- Install engine hooks in post_unpack.
- Do not print before console::is_ready(); use a lock-free file logger instead.
- Apply process-wide tweaks only after the main menu is reached.
- Prefer call-site hooks to prologue detours for hot DB functions.
- Perf and upscaling expectations: on the user's rig BO3 is CPU-bound at about
  200 FPS with the GPU at about 58%, and upstream already removes the hidden
  Com_FPSLimit cap. FSR or DLSS will improve image quality per unit of GPU cost,
  not CPU-bound frame rate. The renderer addresses in the -0xC3D0 region
  (RenderSurfaceList, RenderSurfaceRange, the DrawIndexedInstanced wrapper) are
  starting points for finding injection points and depth / motion-vector
  resources.
- Mod packs and share codes should carry Workshop publisher IDs, versions and
  order, like mod-stack's fs_extramods/modStack advertisement. They must never
  embed extracted third-party assets. Manifests should produce a deterministic
  priority list, and servers should advertise it in getinfo.
- Fix the PoC tools before reusing them for re-verification:
- parameterize the process name (Xerces exe);
- read memory in chunks;
- exclude the second .text and .interpr;
- use capstone for operand-accurate xrefs;
- output a signatures JSON;
- keep dumps local and gitignored.
- Drop legacy-client support (CheckSum 0x888C368) in Xerces. Mod-stack already
  does this, and it simplifies the symbol tables to new client plus server.

## Risks

- The 'PMem_Free ... not at the top of the stack' suppression is unconditional
  (fork patches.cpp:303-308). Porting it would silently leak, or corrupt
  allocator state, in every session.
- The suppression returns from a normally no-return Com_Error, so the engine
  continues after the call site. That is undefined behavior, and it depends on
  the code that follows each Com_Error call.
- The validator patch is applied every launch (fork workshop.cpp:1899-1936). Its
  only guard is a 1-byte check for 0x75, a common byte, so on another build it
  could flip an unrelated jnz.
- bundle_perf_fps_unlock writes the patch byte even when the existing byte is
  unexpected (perf.cpp:243-252). The RSL hook (base+0x1C53FD0) and the
  redundant-asset forwarder (hardcoded 0x142148F60) are also legacy-only; on the
  current exe each would corrupt code or crash.
- Every PoC address is a hardcoded legacy-exe static address and no signatures
  were recorded. The committed dumps and logs are gone, so every claim must be
  re-derived on a fresh live dump.
- Treating the 'queue-add' function (old 0x1412D8810) as an autoexec registrar
  would most likely write into the script string table and corrupt strings or
  crash.
- The file watcher always runs and has no authentication, and bundle_patch is
  reachable through it, so any local process can write arbitrary game memory.
  SendInput automation can also steal focus.
- The CreateThread / NtCreateThreadEx hooks force every thread to start
  suspended even when the hunt is off. Debug-register use interacts with the
  anti-tamper layer.
- Several hooks print on every call (Scr_LoadScript, the Begin/End wrappers, FS
  traces), which costs CPU in the hot path. Printing before the console is ready
  crashed the game.
- The console_logger relies on the single-slot interceptor, so an rcon command
  silently disables file logging.
- bundle_extract_ds4c and the drain path copy and run another mod's compiled
  scripts. A pack or share-code feature built on this would redistribute
  third-party content.
- Memory dumps include .data, which holds live session data. They must never
  leave the machine.
- Upstream mod-stack is 208 commits ahead of the working tree, uses moved paths
  (component/workshop/workshop.cpp, gsc/compiler/...) and is unmerged. Adopting
  it means rebasing or porting, and its API may still change.
- MinHook cannot place two detours on the same target (detour::create throws).
  Xerces modules colliding with upstream components on shared targets (Present,
  Com_Error, DB_LinkXAssetEntry, DB_OverrideAsset, FS_BuildOSPathForThread) will
  fail at startup unless the hooks are multiplexed.
- The PoC's commit messages contain typos and contradictions (section 10 of the
  details). Using their addresses unchecked would target wrong functions.
- Some upstream symbol entries look malformed: db_hashTable{0x149410980,
  146904070} has a decimal literal, and MT_AddMemoryNode{0x01412C8810,
  0x140156A30} puts a server address in the legacy slot. Validate the tables
  before relying on them for Xerces' resolver.
- Many always-on hooks sit next to anti-tamper integrity sites, e.g. the runner
  region at 0x1412D7xxx. Patching code there may trip protection; that area is
  out of scope.

## Open questions

- Should Xerces wait for or rebase onto upstream/mod-stack (c6676a31), or port
  its design independently? This depends on upstream's merge timeline, license
  and attribution expectations, and how far the 208 intervening commits diverge.
- Is the PoC's 'autoexec queue' really the script string table? Falsification
  test on a current-exe dump: check whether SL_GetString (new 0x1412D7B40) or
  SL_GetLowercaseString (new 0x1412D7A30) calls the candidate 0x1412D8830, and
  whether 0x1412C8040 calls 0x1412D7F30 with ecx=1 or 2.
- Do EXPORT_AUTOEXEC functions in custom_scripts fire on the current upstream
  build? Upstream's compiler relies on it, but the PoC observed otherwise in
  April. This needs a live test.
- Does the PMem LIFO fatal still occur with mod-stack's same-batch aliased
  loading, or with stacked mods whose zones share names across tiers?
- Which Steam build or date was the legacy exe (CheckSum 0x888C368)? There is no
  build ID in the repo, and the PoC's memory dumps and logs no longer exist.
- What are the 16 direct callers of the 'queue-add' function (4 of them in
  0x14233xxxx)? The list was never recorded.
- Is upstream's legacy PMem_Free value (0x141439190) correct? It is far from the
  other legacy PMem functions (0x1420D0F40); the new-client value 0x1420C48B0
  sits next to them.
- Is the body of Com_Frame_Try_Block_Function unchanged in the new exe (limiter
  jge at +0x10E)? This only matters if exceeding com_maxfps's own maximum is
  ever wanted.
- Does the dedicated server binary (CheckSum 0x14C28B4) ship with an unencrypted
  .text? If so, scan_xrefs.py could be used on disk for server-side signatures.
- Which zone allocFlags do zm_mod, mp_mod and cp_mod actually use? The PoC
  hot-swapped them with 0x40, while mod-stack uses 0x100 for mode mods. Confirm
  with a zone-table dump on the current exe.

## Fenced (do not modify)

- src/client/component/arxan.cpp (upstream working tree): anti-tamper and
  anti-debug handling (debug-port and debug-flag queries, a CreateThread hook
  the PoC refers to). Fence it and do not modify. It is also the only consumer
  of utils/hardware_breakpoint and utils/signature.
- src/client/component/integrity.hpp: tables of anti-tamper integrity-check
  block addresses for client and server. Do not modify. Treat the listed sites
  as no-patch zones; one site, 0x1412D7E4C, is 0xC4 bytes before the PoC
  'runner' 0x1412D7F10. Which build the list targets was not analyzed.
- src/client/component/steam_proxy.cpp: Steam client proxy, part of Steam
  ownership and identity plumbing. Fence it.
- src/client/game/symbol.hpp:69-73 (symbol::call_safe via
  arxan::detail::set_address_to_call / callstack_proxy_addr): anti-tamper
  call-proxy plumbing. Use as is; do not modify.
- src/common/utils/hardware_breakpoint.{hpp,cpp}: debug-register control whose
  only user is arxan.cpp. Do not reuse it for Xerces features.
- Fork workshop.cpp HW-breakpoint infrastructure (hw_bp_handler,
  arm_hw_bp_write, arm_hw_bp_on_flag, arm_hw_bp_all_threads,
  set_dr0_queue_trap_on_thread; L165-454) and the bundle_arm_queue_bp_now /
  bundle_hunt_queue_writer / bundle_dump_thread_hook_stats commands: they
  manipulate debug registers, the anti-debug surface. Do not port.
- Fork workshop.cpp CreateThread / NtCreateThreadEx hooks (L289-380,
  L3779-3793): they interact with the anti-tamper layer's thread-creation hook,
  as the PoC's own comment says. Do not port.
- Installed BlackOps3.exe second .text section (VA 0x1AA6B000, size 0x2CF0C00)
  and .interpr (VA 0x2EFE000, size 0x9800): not treated as game code here and
  not analyzed. Exclude them from dumps and signature scans and never patch
  them.
- Com_FPSLimit (0x140F7CFD0): the PoC labels it 'Arxan-protected' (unverified).
  Upstream already hooks it in client_patches.cpp:564-567. Keep upstream's hook
  unchanged and add no further patches inside or around it.
- Upstream client_patches.cpp store_tac_protected_allocs (L264-349) and the
  CL_CheckForResend replacement (L512-557): these exist because of Treyarch
  anti-cheat (TAC) and Arxan runtime code mutation. Leave untouched.
- Fork tools/dump_bo3_memory.py: reads the decrypted image of a running game.
  Acceptable only as read-only offline analysis of game code. Never extend it to
  examine or alter protection code, and never ship it to users.
- src/client/component/auth.cpp: client identity (hardware-serial GUID, XUIDs,
  connect auth). Not ownership verification as such, but review before the
  friends/invites backend touches identity.
