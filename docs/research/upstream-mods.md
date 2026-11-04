# Upstream mods, zones, assets and GSC

> Machine-generated research notes from 2026-11-04, kept as evidence for
> `docs/DESIGN.md`. Code was read at upstream `main` 5aa7fac, `mod-stack`
> ac356c3c and the fork `refs/heads/main` 7805319. `path:line` citations refer
> to those commits. Treat claims as leads: anything marked runtime still needs
> the real game.

## Summary

How upstream finds the game: every engine address is a hand-maintained triple of
(current client, legacy client, dedicated server). The triple entry is picked by
the PE header CheckSum (src/client/game/base.cpp:37,42,47; selection at
src/client/game/ptr.hpp:63-84). The installed BlackOps3.exe reports CheckSum
0x6531394 (TimeDateStamp 1786471253, which is 2026-08-11), so upstream uses the
first column everywhere. That covers 3,473 `WEAK symbol<>` definitions under
src/client/game/symbols, 136 inline `select(0x…)` sites in src/client/component,
and hard-coded call-site lists such as client_patches.cpp:84-102 and
workshop.cpp:1244 (`+0x81`). A signature scanner exists
(src/common/utils/signature.hpp, `_sig`), but only arxan.cpp uses it, and that
file is off limits. All engine hooks are installed with MinHook, one detour per
address; a second detour on the same address throws
(src/common/utils/hook.cpp:239-250). So every hook point listed below is pinned
to one exe build, and any hook Xerces adds has to share or take over the detours
upstream already installs.

Mod and usermap model: the engine has exactly one active mod (`active_mod`, an
ActiveMod at 0x15670DD20, plus the single `fs_game` dvar) and one active usermap
(`active_usermap` at 0x15675AA20) (symbols/ugc.hpp:29-30). Upstream replaces the
engine's 128-entry UGC registry with 8192-entry pools that live in the DLL. To
do that it detours 12 UGC functions plus DB_CheckModXFile,
Mods_Lists_GetInfoEntries_Slice, UGC_SetMapLoadingImage and a debug-only
UGC_LoadMod logger (workshop.cpp:1081-1132), backed by game/impl/ugc/ugc.cpp.
Content is discovered from four sources:

- Steam subscriptions: the engine's UGC_LoadManifests calls the detoured
  UGC_LoadManifest_Impl, which uses ISteamUGC GetItemState/GetItemInstallInfo
  (answered by the emulated interface in src/client/steam/interfaces/ugc.cpp
  from steam_proxy) and reads `<installDir>/workshop.json` Type map/mod.
- Folder scans of `<game>/mods/*` and `<game>/usermaps/*` (the `zone/` subfolder
  if present, `zone/workshop.json` for metadata).
- A scan of `<steamapps>/workshop/content/311210/*/workshop.json`.
- steamcmd downloads, which are moved into `./mods/<id>` or `./usermaps/<id>`.

Activation works like this. `loadmod`, Lua `Engine.Mods_SetMod`, or the connect
path (setup_same_mod_as_host) all reach UGC_LoadModByPublisherId_Impl
(ugc.cpp:276-323). It rescans every pool, applies the mod's `assetlimits.txt`,
calls the engine's UGC_LoadMod(localClient, `WorkshopData*`, reloadFS), then
sets `fs_game` to the publisherId without running the dvar's modified callback.
Switching to or from any real mod forces a filesystem reinit
(workshop.cpp:970-978). Usermaps load a pseudo-mod named "usermaps" when no real
mod is active (CL_SetupForNewServerMap_stub, workshop.cpp:197-219), then call
UGC_SetActiveUsermap. A real mod and a usermap can be active together. Which .ff
files come from the mod folder is decided by the engine, gated by
DB_CheckModXFile_Impl: it returns true only if `<activeMod zoneDir>/<name>.ff`
exists (impl/db/xzone/xzone.cpp:11-31). The rest of zone path resolution is
engine-internal and not hooked; FS_BuildOSPathForThread, FS_AddGameDirectory and
FS_Startup are declared but unused. On the network a mod is identified by one
`modId` string. UGC version is always 1, and the client/server script checksum
test is forced to pass (script.cpp:1205,1493).

Zone and asset hooks today. Zone loading is only observed: DB_LoadXAssets is
detoured to detect level-zone unload, and Com_LoadLevelFastFiles to run load
callbacks (com.cpp:17-70). Nothing hooks DB_LinkXAssetEntry, DB_AddXAsset or
DB_UnloadXZone; DB_UnloadXZone is only called directly. "Asset override"
upstream means one DB_FindXAssetHeader detour (script.cpp:1135-1167) that serves
SCRIPTPARSETREE and RAWFILE assets by exact, case-sensitive name from two roots,
`%LOCALAPPDATA%/boiii/data` and `<game>/boiii`. Each root has global,
`<mapname>/` and `<modPublisherId>/` subtrees. Lua rawfiles additionally go
through FFReader, GetRawFile and require hooks. Precedence is first-wins and
partly nondeterministic, because rawfile trees are loaded with
std::execution::par. Rawfile overrides are never removed and are loaded only
from the DB_AssetPoolInit hook. Localized-string overrides are programmatic
only. No other asset types (StringTable, weapons and so on) have any override,
merge or ownership tracking.

GSC/CSC pipeline. Upstream has a full built-in compiler: preprocessor (#define,
#if(n)def, #insert, REGISTER_SYSTEM), lexer, parser, then an emitter producing
T7 PC bytecode (GSC_OBJ magic `80 47 53 43 0D 0A 00 1C`) plus GDB debug data.
There are two trees:

- `scripts/` is compile-only. It overrides stock scripts when the engine looks
  them up by name.
- `custom_scripts/` is compiled, then linked with Scr_LoadScript.

Both run from a patched call to Scr_BeginLoadScripts inside the server-side
game-script loader (script.cpp:1250-1266, 1489). autoexec is just the export
flag 2; the engine runs those functions. Upstream never calls main or init.
Detours are a single
`std::unordered_map<target bytecode address, replacement address>`, filled by
compile-time `detour` syntax or the runtime `replacefunc`. It is consulted by
patched VM handlers for the SafeCreateLocalVariables and CheckClearParams
opcodes (gsc/vm.cpp:94-117, 225-227). Limits: server VM only, one replacement
per target, no way to call the original, and the map is cleared globally by
clearreplacefuncs and on every G_InitGame/G_ShutdownGame. Custom builtins work
by detouring Scr_GetFunction/Scr_GetMethod and the CScr equivalents.

Limits and fastdl:

- Asset pools only ever grow. Sizes come from launcher_settings.json `ap_*`
  keys, a mod's or usermap's `assetlimits.txt`/`assetpools.txt`, and automatic
  doubling when a pool runs out. The reallocation routine copies only the first
  `itemAllocCount` slots and calls free() on a previous heap extension
  (impl/db/db.cpp:411-451). Live asset headers can still point into that freed
  memory, which looks like a use-after-free.
- The zone table has 64 slots, and the engine errors at 64, so 63 are usable.
  Each script VM tracks at most 500 linked script objects.
- fastdl covers usermaps only. The URL comes from the server's `sv_wwwBaseURL`.
  The manifest is an unsigned JSON array of [name, size, sha1]. Map-type files
  that already exist are considered up to date on size alone, there is no
  file-extension allowlist, and the sv_wwwDownload dvar is never checked.

The fork's proof of concept (refs/heads/main, old exe) adds runtime evidence of
the core blocker for additive loading. Every mod ships zones with the same names
(core_mod/en_core_mod, plus zm_mod/mp_mod/cp_mod) in the mod tier, allocFlags
0x40. DB_LoadXAssets automatically adds an `en_` language-pair prefix. Zone
memory is a LIFO stack, so the same-named zones from a second mod produce
"PMem_Free … not at the top of the stack". Xerces will need its own symbol table
built from pattern scans, a central hook dispatcher, deterministic ordering, an
asset-ownership hook (DB_LinkXAssetEntry), a typed override/merge registry and a
chaining GSC hook registry. It also needs a zone-loading strategy that works
around the single-mod engine, without touching fastfile authentication or Arxan.

## Facts

- **verified**: Upstream selects one of three address columns by PE header
  CheckSum: 0x6531394 = current client, 0x888C368 = legacy client, 0x14C28B4 =
  dedicated server. The installed BlackOps3.exe has CheckSum 0x6531394
  (TimeDateStamp 1786471253, which is 2026-08-11), so the first column is the
  live one. (src/client/game/base.cpp:37,42,47; src/client/game/ptr.hpp:63-84
  (select_value/select); PE header of <local scratch copy, not kept>Files
  (x86)/Steam/steamapps/common/Call of Duty Black Ops III/BlackOps3.exe read
  with Python)
- **verified**: All engine addresses are hard-coded: 3,473 WEAK symbol triples,
  136 inline select(0x..) sites in components, and literal call-site lists. The
  only pattern scanner (utils::hook::signature / _sig) is used solely by
  arxan.cpp. (grep counts over src/client/game/symbols and src/client/component;
  client_patches.cpp:84-102; workshop.cpp:1244 (offset 0x81); arxan.cpp:636-696;
  src/common/utils/signature.hpp:5-46)
- **verified**: Detours are MinHook-based. Creating a second detour on an
  already-hooked address throws, so each engine function can have only one
  owner. (src/common/utils/hook.cpp:239-250 (MH_CreateHook != MH_OK -> throw))
- **verified**: The engine state holds one active mod (ActiveMod with loadState)
  and one active usermap. Upstream code only ever consults active_mod for mod
  fastfile and script resolution. (symbols/ugc.hpp:29-30; structs/ugc.hpp:57-68;
  impl/db/xzone/xzone.cpp:12-24; script.cpp:751-754; gdb.cpp:180-220)
- **verified**: Upstream replaces the engine's 128-entry mods/usermaps
  registries with 8192-entry pools that live in the DLL, by detouring
  UGC_GetByPublisherId, GetCount, VerifyVersion, LoadPool, LoadModsPool,
  LoadUsermapsPool, LoadPools, LoadModByPublisherId,
  SetMapPreviewImageByPublisherId, LoadManifest, LoadUsermapByPublisherId and
  DB_CheckModXFile, plus (client) Mods_Lists_GetInfoEntries_Slice and
  UGC_SetMapLoadingImage. (workshop.cpp:1081-1132; structs/ugc.hpp:196-208;
  symbols/ugc.cpp:9-10)
- **verified**: Mod pool fill order: (server only) the downloaded-mod manifest,
  then the engine's UGC_LoadManifests (Steam subscriptions via the detoured
  UGC_LoadManifest_Impl), then a scan of `<cwd>/mods` subdirectories, then
  supplement_mods_from_disk (only if the pool is still empty), then a scan of
  `<steamapps>/workshop/content/311210/*/workshop.json`, then workshop.json
  metadata for the folder-scan entries. Usermaps follow the same pattern
  (playlist manifests on server). (ugc.cpp:170-257 (order), 89-106 (patches),
  108-168 (scan), 425-539 (Steam manifest); workshop.cpp:276-422)
- **verified**: Steam-subscription entries and directory-scanned Workshop
  entries are not de-duplicated. supplement_ugc_from_workshop's early-return
  checks usermapsPool.count even when filling the mods pool.
  (workshop.cpp:314-318 and 349-414 (append without existence check))
- **verified**: workshop.json for local items must contain Title, Description,
  FolderName and PublisherID. Workshop-folder items also need Type 'mod' or
  'map'. PublishedFileId is optionally used to resolve download IDs.
  (workshop.cpp:241-253, 370-408, 147-165; ugc.cpp:463-503)
- **verified**: Local layout is <game>/mods/<dir>/zone/ and
  <game>/usermaps/<dir>/zone/, falling back to <dir> when zone/ is missing.
  Steam Workshop items use the item root as absolutePathZoneFiles.
  (workshop.cpp:447-469 (va_mods_path / va_user_content_path); ugc.cpp:139-156,
  505-518; workshop.cpp:375-386)
- **verified**: Mod activation: UGC_LoadModByPublisherId_Impl rescans all pools,
  synthesizes an entry for unknown IDs (mods/<id>/zone), applies the first
  assetlimits.txt/assetpools.txt in the zone tree, calls the engine's
  UGC_LoadMod(localClient, mod, reloadFS), then sets fs_game to the publisherId
  without running the modified callback. (ugc.cpp:276-323, 259-274;
  structs/dvar.hpp:1133-1136 (execModifiedCallback param);
  symbols/dvar/core.hpp:49-50)
- **verified**: Entry points into mod activation: the 'loadmod' console command,
  the Lua Engine.Mods_SetMod detour (immediate load, reloadFS=true), and connect
  via setup_same_mod_as_host (force_fs_reinit=true). (workshop.cpp:1197-1209,
  986-1011; ui_scripting.cpp:2298-2313, 2553-2555; party.cpp:70-80)
- **verified**: Any switch to or from a real mod (not 'usermaps' and not empty)
  requires a filesystem reinit. wait_for_mod_load busy-polls
  active_mod->loadState every 100 ms. (workshop.cpp:970-984)
- **verified**: DB_CheckModXFile_Impl returns true only if
  '<activeMod.absolutePathZoneFiles>/<name>.ff' (fallback
  '<cwd>/mods/<publisherId>/<name>.ff') opens via Stream_OpenFile(path, 11). How
  the engine consumes this result is not visible in upstream code.
  (impl/db/xzone/xzone.cpp:11-31; symbols/stream.hpp (Stream_OpenFile
  0x142122480))
- **verified**: Usermaps: CL_SetupForNewServerMap_stub loads the pseudo-mod
  'usermaps' (reloadFS=false) when the map is a usermap and no mod is loaded.
  When the map is not a usermap it unloads 'usermaps' and
  DB_UnloadXZone('zm_levelcommon'). A real mod stays active alongside a usermap.
  (workshop.cpp:197-219, 173-195)
- **verified**: The UGC_LoadUsermapByPublisherId detour maps internal names to
  publisherIds, applies the usermap's assetlimits, then calls the engine's
  UGC_SetActiveUsermap. (workshop.cpp:424-433; ugc.cpp:541-551)
- **verified**: Zone-loading hooks are observational only. The DB_LoadXAssets
  detour detects a level-zone unload (name==nullptr, allocFlags==0 or contains
  mask 0x64, zoneCount==1) and runs on_level_unload. The Com_LoadLevelFastFiles
  detour runs on_level_load. The only subscriber is currency.cpp.
  (com.cpp:17-27, 39-70, 99-102; currency.cpp:1136-1137)
- **verified**: No upstream hook exists on DB_LinkXAssetEntry, DB_AddXAsset or
  DB_UnloadXZone. DB_LinkXAssetEntry is declared but never used.
  (symbols/db/load.hpp:21-23; grep over src/client shows no .create on these)
- **likely**: XAssetEntry carries zoneIndex plus nextHash/nextType/nextOverride
  chain indices, which is the engine's per-zone ownership and override chain.
  (structs/db/xasset/core.hpp:968-977)
- **verified**: Upstream 'asset override' is a DB_FindXAssetHeader detour. Only
  SCRIPTPARSETREE (from loaded_scripts) and RAWFILE (from loaded_rawfiles) are
  served, by exact case-sensitive name, before falling through to the engine.
  With -extract-assets, looked-up assets are dumped. (script.cpp:1135-1167,
  1482-1483; dump.cpp:32-133; flags.cpp:7-33)
- **verified**: Rawfile override trees: lua, '' (root, non-recursive),
  aim_assist, con_restrict, devgui, fpstool, gamedata, leaderboards, radiant,
  scriptbundle, ui, ui_mp, vehicle, video, vision, zone, zone_source (+ lui on
  client). Each tree also exists as <mapname>/<tree> and <modPublisherId>/<tree>
  with the prefix stripped, under %LOCALAPPDATA%/boiii/data and <game>/boiii.
  (script.cpp:1052-1122)
- **verified**: Rawfile override precedence is first-inserted-wins. Trees and
  files load in parallel (std::execution::par), so global vs map vs mod
  precedence is nondeterministic. Within one tree, appdata is loaded before the
  game folder. Entries are never replaced or cleared, so overrides persist for
  the whole process. (script.cpp:895-930 (insert-if-absent; comment 897-908),
  1046-1047, 1060-1068, 1118-1121; clear_script_memory 1169-1179 omits
  loaded_rawfiles)
- **verified**: Rawfile overrides (including .lua sources compiled to HKS
  bytecode in-process) are loaded only from the DB_AssetPoolInit detour.
  (asset_limits.cpp:262-268; script.cpp:955-1003)
- **verified**: Lua rawfile overrides are also enforced at the FFReader level,
  and local ui_scripts files are served through detours on Lua_CoD_GetRawFile,
  the hks_load call site and package.require. (ui_scripting.cpp:1199-1250,
  2175-2201, 2561-2581)
- **verified**: ui_scripts: every `<root>/ui_scripts/<dir>/__init__.lua` from
  both roots is executed at UI init (lobby_scripts/ on server). In the
  local-script filename map the game folder entry overwrites the appdata entry,
  which is the opposite precedence from GSC. (ui_scripting.cpp:526-566,
  1034-1041, 1078-1095)
- **verified**: Built-in GSC compiler: preprocess (#define object/function-like,
  #ifdef/#ifndef/#else/#endif/#undef, #insert search path,
  `__FILE__/__LINE__/__DATE__/__TIME__`, REGISTER_SYSTEM -> autoexec
  `__init__sytem__`), then tokenize, parse, generate the addcommand dispatcher,
  and emit T7 bytecode plus GDB. Devblocks /# #/ are stripped before compiling.
  (gsc_compiler.cpp:29-84, 92-572, 689-803, 806-859; script.cpp:120-173;
  structs/scr/core.hpp:1748-1909 (T7_LATEST_VERSION 0x1C))
- **verified**: scripts/ is a compile-only override tree served by name.
  custom_scripts/ is compiled and then linked via Scr_LoadScript, with the VM
  instance chosen by file extension (.csc is skipped on dedicated servers).
  (script.cpp:888-893, 604-642, 416-454, 456-602)
- **verified**: Script tree order: shared subdirectories (from an
  unordered_set), then <mode> (map subdirectories excluded), then <map>, then
  <mode>/<map>, then <modPublisherId>, then the tree root (non-recursive);
  appdata before game folder. The first occurrence of a lower-cased name wins,
  so a mode-generic override beats a map- or mod-specific one with the same
  name. (script.cpp:803-886 (push order 828-865), 711-717 (processed_scripts
  dedupe), 756-801)
- **verified**: The script injection point is the call to Scr_BeginLoadScripts
  inside the server-side game-script loader at select(0x141AA255F, 0x141AAE92F,
  0x1402D81FF). It only runs when Com_IsInGame() and not the UI level. It
  compiles and loads trees, binds pending detours and rebuilds
  <game>/zone/scriptgdb.zip. (script.cpp:1250-1266, 1489-1490, 1181-1203; commit
  857c259c (replaced the earlier GScr_LoadGametypeScript hook))
- **verified**: Compile-time detours ('detour ns<path>::fn(...) {}' definitions
  and 'detour(target, repl);' statements) are queued and bound by resolving
  export hash plus param count in the target and replacement GSC_OBJs.
  Unresolved ones wait until the next script load. (gsc_emitter.cpp:1290-1336,
  2172-2228; script.cpp:525-544, 321-380, 176-311)
- **verified**: Detour runtime: gs<local scratch copy, not
  kept>:function_replacements is a plain `unordered_map<uint8_t*, uint8_t*>`
  (last registration wins). The VM handlers for SafeCreateLocalVariables and
  CheckClearParams rewrite fs->pos when the opcode address is a key, only for
  SCRIPTINSTANCE_SERVER. There is no chaining or call-original. The map is
  cleared by clearreplacefuncs, G_InitGame and G_ShutdownGame.
  (gsc_funcs.cpp:40-43, 491-508, 1540-1543, 1798-1811; gsc/vm.cpp:94-117,
  225-227)
- **likely**: autoexec is an export flag (EXPORT_AUTOEXEC=2, PRIVATE=4) set by
  the compiler. Upstream never invokes main or init. Running autoexecs is left
  to the engine's linker/end-load step. (gsc_emitter.hpp:23-27;
  gsc_emitter.cpp:1780-1781; gsc_parser.cpp:1087-1094, 1288-1307; no
  'main'/'init' calls in components; fork commit d34d95d5 maps an autoexec queue
  and runner on the old exe)
- **verified**: Custom GSC/CSC builtins are added by detouring
  Scr_GetFunction/Scr_GetMethod, CScr_GetFunction/CScr_GetMethod and their
  reverse lookups. (gsc_funcs.cpp:1410-1515, 1602-1626;
  symbols/scr/builtin/core.hpp:10,15; cscr.hpp:11,16)
- **verified**: Consistency checks are relaxed: CG_TestServerScriptChecksum
  always returns 1, the BG cache client/server index-mismatch branch is forced,
  and UGC version is always 1 and only compared for equality. (script.cpp:1205,
  1493-1495; client_patches.cpp:482-486; ugc.cpp:75-79 and every 'version = 1'
  assignment)
- **verified**: Engine 'mod loaded' checks are forced false (Scr_IsModLoaded
  jump plus 20 hard-coded call sites per client build) to keep the client
  ranked. (client_patches.cpp:81-108, 473-480)
- **verified**: Each script VM tracks at most 500 linked script objects
  (ObjFileInfoPool = ScrPool<array<objFileInfo_t,500>>). The SCRIPTPARSETREE
  pool default is 0x47E. (structs/scr/core.hpp:2097-2098; impl/db/db.cpp:333;
  asset_limits.cpp:143)
- **verified**: The zone table has 64 slots and the engine errors at 64
  (MAX_ZONE_COUNT 63). XZoneName.zoneType only distinguishes OFFICIAL, MOD and
  USERMAP. (structs/db/xzone.hpp:14-18, 73-85; structs/core.hpp:544-560)
- **verified**: Asset pools grow from launcher_settings.json '`ap_*`' keys at
  DB_AssetPoolInit, from mod/usermap assetlimits.txt/assetpools.txt (both the
  T7Recharged 4-column and the 2-column syntax), and by doubling in
  DB_AssetPoolAlloc_Impl when the free list is exhausted. Pools never shrink.
  (asset_limits.cpp:18-71, 195-268; ugc.cpp:259-274; impl/db/db.cpp:486-515)
- **likely**: reallocate_asset_pool memcpy's only the first itemAllocCount
  slots, rebuilds the free list from that index, and free()s the previous
  heap-extended pool while existing XAssetEntry headers can still point into it.
  On the dedicated server, IMAGE allocations return freeHead without advancing.
  (impl/db/db.cpp:386-457 (411-451), 491-495)
- **likely**: Per the fork PoC (old exe): mods ship same-named zones
  (core_mod/en_core_mod, zm_mod/cp_mod/mp_mod) in the mod tier (allocFlags 0x40;
  en_ pair 0x8000040). DB_LoadXAssets auto-prefixes 'en_' and needs the pair in
  the same tier. Loading into a fresh tier fails with 'Could not find zone
  en_X'. Same-named zones in different slots trigger 'PMem_Free ... not at the
  top of the stack'. Hot-swap works via FS search-path promotion plus reloading
  the tier. (fork commit 88912f57 (message); fork
  src/client/component/patches.cpp:285-307; fork workshop.cpp:1601-1640,
  2556-2599)
- **verified**: Loader-relevant engine symbols are declared but unused:
  FS_AddGameDirectory 0x142245DB0, FS_Startup 0x1422478E0,
  FS_BuildOSPathForThread 0x1420C9020 (signature includes ZoneType and
  zone_internal_id), DB_LinkXAssetEntry 0x1414228E0, g_zoneInfo/g_zoneInfoCount,
  `UGC_ModChange_*`, UGC_ModLoad_TryComplete. (symbols/fs.hpp:35-47;
  symbols/db/load.hpp:21-23; symbols/db/xzone.hpp:18-20; symbols/ugc.hpp:26-27,
  60-65; grep shows no callers)
- **verified**: fastdl triggers only for a missing usermap (not a stock fastfile
  and not installed) when the server advertises sv_wwwBaseURL. It GETs
  <base>/usermaps/<map>/manifest.json (a JSON array of [name, size, sha1]),
  downloads each file to ./usermaps/<map>/<name> with size and SHA1 checks,
  downloads workshop.json last, then rescans pools. (workshop.cpp:776-815;
  party.cpp:251-291; getinfo.cpp:148-157; fastdl.cpp:120-180, 182-309, 370-437)
- **verified**: fastdl integrity is limited. The manifest is unsigned and the
  SHA1s come from the same server. Existing .dll/.xpak/.ff/.json/.sabs/.sabl
  files count as current if the size matches. There is no extension allowlist
  (only a relative-path/'..' guard). Redirects are followed, TLS verification is
  on, and there is no timeout. sv_wwwDownload and sv_wwwDlDisconnected are
  registered but never read. (fastdl.cpp:68-109, 188-191, 224-238, 331-354;
  getinfo.cpp:77-81; grep for fastdl_enabled/wwwDownload)
- **verified**: Mods are never downloaded via fastdl. Missing mods and maps go
  through steamcmd with '+login anonymous app_update 311210
  +workshop_download_item 311210 <id> validate' and are moved into ./mods/<id>
  or ./usermaps/<id>. The launcher browses the Workshop by scraping
  steamcommunity HTML. (workshop.cpp:873-968; steamcmd.cpp:648-781 (736-738);
  launcher_workshop.cpp:794-912)
- **verified**: The updater deletes any file under %LOCALAPPDATA%/boiii/data
  that is not in its manifest. That is the same root used for scripts/,
  custom_scripts/ and ui_scripts/ overrides. (updater.cpp:47 (base = appdata);
  file_updater.cpp:190-201, 674-712)
- **verified**: On the network a mod is identified by one string: modId = the
  active publisherId. workshop_id/usermapId come from one dvar. The client
  matches modId against pool publisherId or internalName. (getinfo.cpp:148-157;
  workshop.cpp:107-116, 523-537)
- **verified**: The engine exposes a Lua Mods API (Mods_InstalledUGC,
  InstallProgressUGC, IsSubscribedItem, `Lists_*`, Load_Updating, SetMod,
  SubscribeUGC). Upstream hooks only SetMod. The stock Mods menu data source is
  the Mods_Lists_GetInfoEntries_Slice detour, refreshed via the UI models
  ModsLoadEntry.update, UsermapsEntries.update and ModsGlobal.update.
  (symbols/lua/cod/api.hpp:2626-2649; impl/lua/lua.cpp:7-47; ugc.cpp:174-209,
  216-251)
- **verified**: The Mods_SetMod detour captures a raw const `char*` from the Lua
  stack into a deferred scheduler lambda. (ui_scripting.cpp:2298-2313)
- **verified**: Upstream ships its own stock-script overrides in the same
  override namespace mods use (data/scripts/mp/bots/_bot.gsc, _bot_loadout.gsc,
  gametypes/_serversettings.gsc, teams/_teams.gsc) plus about 140 .gsh headers
  for #insert. (`data/scripts/**` listing (144 files))
- **verified**: For 12 publisher IDs (KNOWN_MALICIOUS_MODS) Engine.IsBOIII and
  IsEZZBOIII are hidden, and Com_GetBuildInt/StringField are spoofed for Lua to
  defeat mods' client detection. (ui_scripting.cpp:966-1029, 2543-2552)
- **verified**: Localized-string overrides are a programmatic map behind an
  SEH_StringEd_GetString detour at select(0x14221CBC0, 0x1422796E0, 0). Its only
  caller is demonware.cpp. (localized_strings.cpp:15-31, 42-43;
  demonware.cpp:451)
- **verified**: Two declared symbols have suspicious values and are unused:
  db_hashTable's legacy address is decimal 146904070 (0x8C19406) with no server
  address, and DB_AssetPoolRelease's legacy address equals its server address
  0x1401A15A0. (symbols/db/core.hpp:10; symbols/db/xasset/pool.hpp:10-11)
- **likely**: DB_GetAssetEntryPoolEntryByName starts at g_assetEntryPool[hash %
  0x263FF] and walks the free-list 'next' union member instead of
  nextHash/db_hashTable, so it cannot be used for ownership lookups. It is
  unused. (impl/db/db.cpp:459-475; structs/db/xasset/pool.hpp:97-107)

## Details

Repo root: `<local scratch copy, not kept>ve/Desktop/Environment/Xerces` (branch
`xerces` = upstream 5aa7fac). All `path:line` below are repo-relative. "Client
addr" = first column of the symbol triple (live for the installed exe, PE
CheckSum 0x6531394).

## A. Address model (everything below depends on it)

| Item              | Where                                                                                                 | Notes                                                                              |
| ----------------- | ----------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Build detection   | `src/client/game/base.cpp:37,42,47`                                                                   | CheckSum 0x6531394 = current client; 0x888C368 = legacy client; 0x14C28B4 = server |
| Address selection | `src/client/game/ptr.hpp:35-84`, `symbol.hpp:5-75`                                                    | `symbol<T>{client, legacy, server}`, relocated against the host module             |
| Scale             | 3,473 `WEAK symbol<>` in `src/client/game/symbols/**`; 136 `select(0x…)` in `src/client/component/**` | manually re-dumped per game patch (commits 42a5d818, 73cc97dc, 4b4fd331)           |
| Pattern scanner   | `src/common/utils/signature.hpp:5-46` (`"…"_sig`)                                                     | only used in `arxan.cpp:636-696` (off limits)                                      |
| Detour engine     | `src/common/utils/hook.cpp:239-250` (MinHook)                                                         | one detour per address; a second `create` throws                                   |

### Key loader addresses (current client column)

| Symbol                                                                                       | Addr                                    | Defined at                            |
| -------------------------------------------------------------------------------------------- | --------------------------------------- | ------------------------------------- |
| `DB_LoadXAssets(XZoneInfo*, count, sync, suppressSync)`                                      | 0x1414236C0                             | symbols/db/load.hpp:18-20             |
| `DB_LinkXAssetEntry(XAssetEntry*, allowOverride)`                                            | 0x1414228E0                             | load.hpp:21-23 (unused)               |
| DB_LoadXFile(...)                                                                            | 0x1413EF6F0                             | load.hpp:25-28                        |
| DB_UnloadXZone(zoneIdx, createDefault, suppressSync)                                         | 0x141425A90                             | load.hpp:29-30                        |
| DB_ReleaseXAssets                                                                            | 0x1414247E0                             | load.hpp:31                           |
| DB_DelayedCopyXAsset                                                                         | 0x14141FF20                             | load.hpp:14-15                        |
| g_load (DB_LoadData)                                                                         | 0x14938D3C0                             | load.hpp:17                           |
| DB_FindXAssetHeader(type, name, errIfMissing, wait)                                          | 0x141420EF0                             | symbols/db/xasset/core.hpp:13-15      |
| DB_EnumXAssets / DB_IsXAssetDefault                                                          | 0x141420990 / 0x141422160               | xasset/core.hpp:10-21                 |
| DB_AssetPoolInit / DB_AssetPoolAlloc                                                         | 0x1413E9F00 / 0x1413E9E50               | db/core.hpp:12; xasset/pool.hpp:12-13 |
| s_assetPools / g_assetEntryPool                                                              | 0x1493883F0 / 0x149448B80               | xasset/pool.hpp:15-17                 |
| DB_ValidFastFile(name, source)                                                               | 0x141420B60                             | db/core.hpp:13-14                     |
| DB_CheckModXFile(ffName)                                                                     | 0x1420C9260                             | db/xzone.hpp:22-23                    |
| g_zoneNames / g_zoneCount                                                                    | 0x149910B80 / 0x14939197C               | xzone.hpp:12-15                       |
| g_zoneInfo / g_zoneInfoCount                                                                 | 0x149916020 / 0x149917620               | xzone.hpp:18-20                       |
| Com_LoadLevelFastFiles / Com_UnloadLevelFastFiles                                            | 0x1420EFA80 / 0x1420F2000               | symbols/com/core.hpp:53-58            |
| UGC_LoadMod(lc, `WorkshopData*`, reloadFS)                                                   | 0x1420CA3D0                             | symbols/ugc.hpp:20-22                 |
| UGC_LoadModByPublisherId                                                                     | 0x1420CA1B0                             | ugc.hpp:23-25                         |
| UGC_LoadManifest / UGC_LoadManifests                                                         | 0x1420C8920 / 0x1420CA0C0               | ugc.hpp:43-47                         |
| UGC_LoadUsermapByPublisherId / UGC_SetActiveUsermap                                          | 0x1420C8F80 / 0x1420C8DE0               | ugc.hpp:56-59                         |
| active_mod / active_usermap                                                                  | 0x15670DD20 / 0x15675AA20               | ugc.hpp:29-30                         |
| builtinModsPool / builtinUsermapsPool                                                        | 0x15670E1F0 / 0x156734600               | ugc.hpp:9-14                          |
| FS_AddGameDirectory / FS_Startup                                                             | 0x142245DB0 / 0x1422478E0               | symbols/fs.hpp:35-41 (unused)         |
| FS_BuildOSPath / FS_BuildOSPathForThread(ospath, ext, len, game, ZoneType, zone_internal_id) | 0x142246220 / 0x1420C9020               | fs.hpp:42-47 (ForThread unused)       |
| Sys_Cwd / Sys_ListFiles / Sys_GetAbsZoneDir                                                  | 0x142247CE0 / 0x14227D940 / 0x142247CF0 | symbols/sys.hpp:27-38                 |
| Stream_OpenFile                                                                              | 0x142122480                             | symbols/stream.hpp:9-10               |
| Scr_LoadScript(inst, filename) / Scr_BeginLoadScripts(inst, user)                            | 0x1412C8410 / 0x1412C7E10               | symbols/scr/core.hpp:79-82            |
| Scr_BeginLoadScripts call site (patched)                                                     | 0x141AA255F                             | script.cpp:1489                       |
| Scr_GetFunction / Scr_GetMethod                                                              | 0x141AEB450 / 0x141AEB5E0               | symbols/scr/builtin/core.hpp:10,15    |
| gObjFileInfo / gObjFileInfoCount                                                             | 0x14505D2E0 / 0x145070B60               | scr/core.hpp:171-174                  |
| CL_SetupForNewServerMap (+0x81 call patched) / CL_PreloadMap                                 | 0x14135CD40 / 0x14135A200               | symbols/cl.hpp:22,32                  |
| Mods_Lists_GetInfoEntries_Slice                                                              | 0x1420C9A60                             | symbols/lua/core.hpp:105              |
| Lua_CoD_LuaCall_Mods_SetMod / SubscribeUGC                                                   | 0x141FDCEB0 / 0x141FDCEE0               | symbols/lua/cod/api.hpp:2646-2649     |
| Lua_CoD_GetRawFile / Lua_CoD_FFReader                                                        | 0x141F02860 / 0x141F027E0               | symbols/lua/cod/core.hpp:12,24        |
| fs_game dvar                                                                                 | 0x1579E63F8                             | symbols/dvar/core.hpp:49-50           |
| G_InitGame (Rope_InitRopes call) / G_ShutdownGame (Mantle_ShutdownAnims call)                | 0x1419D15F9 / 0x1419F66DD               | game_event.cpp:56-59                  |

## B. Content discovery and selection flow

### Where content is searched (in fill order) — `ugc.cpp:170-257`

1. **Server only:** UGC_DownloadedModPublisherId, then UGC_LoadManifest_Impl
   (`ugc.cpp:189-193`).
2. **Steam subscriptions:** the engine's `UGC_LoadManifests` calls the detoured
   `UGC_LoadManifest_Impl` (`ugc.cpp:425-539`).
   - It uses `ISteamUG<local scratch copy, not kept>:GetItemState` /
     `GetItemInstallInfo`, answered by
     `src/client/steam/interfaces/ugc.cpp:247-300` from `steam_proxy` subscribed
     items.
   - It reads `<installDir>/workshop.json`, where `Type` "map" or "mod" picks
     the pool.
   - The resulting entry has `absolutePathZoneFiles=<installDir>` and
     `publisherIdInteger=<id>`.
3. **Folder scan:** `UGC_LoadPool_Impl` lists the subdirectories of `<cwd>/mods`
   or `<cwd>/usermaps` (`Sys_ListFiles(dir,"/")`).
   - The folder name is used as title, internalName and publisherId.
   - zoneDir is `<dir>/zone`, falling back to `<dir>` (`ugc.cpp:108-168`,
     `workshop.cpp:459-469`).
4. **Patches** (`ugc.cpp:89-106`):
   - `supplement_mods_from_disk` runs only if the mods pool is still empty
     (`workshop.cpp:276-312`).
   - `supplement_ugc_from_workshop` scans
     `<cwd>/../../workshop/content/311210/*/workshop.json` by Type
     (`workshop.cpp:314-422`, no de-dupe).
   - `load_workshop_data` fills Title, Description, FolderName and PublisherID
     for folder-scan entries (`workshop.cpp:221-253`).
5. **steamcmd downloads:** `./steamcmd/steamapps/workshop/content/311210/<id>`
   is moved to `./mods/<id>` or `./usermaps/<id>` (`steamcmd.cpp:656-775`).

### Mod activation

```
loadmod <id|folder> (workshop.cpp:1197)  |  Lua Engine.Mods_SetMod(id) (ui_scripting.cpp:2298)  |  connect -> setup_same_mod_as_host (workshop.cpp:986)
  -> UGC_LoadModByPublisherId [engine 0x1420CA1B0, detoured] -> UGC_LoadModByPublisherId_Impl (ugc.cpp:276)
     1 UGC_LoadPools_Impl()            // full rescan, every call (also on every usermap map load)
     2 lookup by publisherId; unknown -> UGC_DownloadModByPublisherId (client stub=0) -> synthesize mods/<id>/zone entry
     3 UGC_LoadItem_PrepareAssetPool(zoneDir)   // first assetlimits.txt|assetpools.txt, recursive (ugc.cpp:259-274)
     4 engine UGC_LoadMod(lc, mod, reloadFS)    // fills active_mod; FS restart when reloadFS
     5 fs_game->set(publisherId, false)          // no modified-callback (no queued UGC_ModChange)
reloadFS policy: true unless both old/new are "" or "usermaps" (workshop.cpp:970-978); busy-wait on active_mod->loadState (980-984)
```

### Connect / usermap flow

```
infoResponse (getinfo.cpp:148-157): modName, modId, sv_wwwBaseURL(+Url), workshop_id, usermapId
party.cpp:251-291 -> check_valid_usermap_id(map, localPubId, workshop_id, base_uri) (workshop.cpp:776-871)
     !DB_ValidFastFile(map,0) && not installed: ZM DLC -> popup+URL | base_uri -> fastdl | numeric -> steamcmd prompt | else error
  -> check_valid_mod_id(modId, workshop_id) (873-968): !has_mod -> steamcmd prompt
  -> connect_to_lobby -> setup_same_mod_as_host(usermap, mod, force_fs_reinit=true)
engine CL_SetupForNewServerMap [detoured, workshop.cpp:197-219]:
     usermap && !mod -> load pseudo-mod "usermaps" (reloadFS=false)
     !usermap && "usermaps" active -> load "" ; and DB_UnloadXZone(index of "zm_levelcommon")
     +0x81 Com_Error("missing map") -> com_error_missing_map_stub: auto-reconnect once after 3 s (1016-1053)
engine -> UGC_LoadUsermapByPublisherId [detoured] -> internal name->pubId, assetlimits, UGC_SetActiveUsermap (ugc.cpp:541-551)
engine loads map/mod zones (engine-internal; mod .ff override gate = DB_CheckModXFile_Impl)
G_InitGame -> server script loader -> [patched Scr_BeginLoadScripts call] -> scripts/ + custom_scripts/ -> Scr_LoadScript(custom) -> bind detours
```

## C. Hook catalog (upstream, current client addresses)

| #    | Engine target                                                                                                                                                                                     | Hook site                                       | Behaviour                                                                          |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- | ---------------------------------------------------------------------------------- |
| 1    | DB_LoadXAssets 0x1414236C0                                                                                                                                                                        | com.cpp:39-70, 99-100                           | pass-through; detects level-zone unload -> `com::on_level_unload(g_mapname)`       |
| 2    | Com_LoadLevelFastFiles 0x1420EFA80                                                                                                                                                                | com.cpp:17-27, 101-102                          | after original -> `com::on_level_load(level)`                                      |
| 3    | DB_CheckModXFile 0x1420C9260                                                                                                                                                                      | workshop.cpp:1092 -> xzone.cpp:11-31            | full replace: `<activeMod zoneDir>/<ff>.ff` exists?                                |
| 4-14 | UGC_GetByPublisherId, GetCount, VerifyVersion, LoadPool, LoadModsPool, LoadUsermapsPool, LoadPools, LoadModByPublisherId, SetMapPreviewImageByPublisherId, LoadManifest, LoadUsermapByPublisherId | workshop.cpp:1094-1118 -> ugc.cpp               | registry re-implementation on 8192 pools                                           |
| 15   | Mods_Lists_GetInfoEntries_Slice 0x1420C9A60 (client)                                                                                                                                              | workshop.cpp:1121-1123 -> impl/lua/lua.cpp:7-47 | Mods/usermaps menu list (name, internalName, description, ugcName, ugcVersion)     |
| 16   | UGC_SetMapLoadingImage (client)                                                                                                                                                                   | workshop.cpp:1125-1126 -> ugc.cpp:361-391       | `<usermap zone>/loadingimage.png` -> img_t7_mod_loading                            |
| 17   | UGC_LoadMod (debug only)                                                                                                                                                                          | workshop.cpp:1056-1065, 1130                    | trace logging                                                                      |
| 18   | CL_SetupForNewServerMap 0x14135CD40                                                                                                                                                               | workshop.cpp:197-219, 1240-1242                 | usermaps pseudo-mod; zm_levelcommon unload                                         |
| 19   | CL_SetupForNewServerMap+0x81 (call)                                                                                                                                                               | workshop.cpp:1244-1245                          | missing-map error -> reconnect to download                                         |
| 20   | CL_PreloadMap 0x14135A200                                                                                                                                                                         | client_patches.cpp:51-56, 471                   | set gametype settings; stop ZM/CP intro                                            |
| 21   | Scr_IsModLoaded (jump) + 20 call sites                                                                                                                                                            | client_patches.cpp:81-108, 473-480              | engine sees "no mod loaded"                                                        |
| 22   | DB_AssetPoolInit 0x1413E9F00                                                                                                                                                                      | asset_limits.cpp:262-268, 280-281               | after init: apply pool sizes, DB_InitBSPGlobals_Impl, `script::load_rawfiles()`    |
| 23   | DB_AssetPoolAlloc 0x1413E9E50                                                                                                                                                                     | asset_limits.cpp:282-284 -> db.cpp:486-515      | mutex + auto-double on exhaustion                                                  |
| 24   | Com_GetMapTable 0x142247EC0                                                                                                                                                                       | db.cpp:55-76, 91-92                             | default MapTable when missing/default                                              |
| 25   | `Com_GametypeSettings_*` (3)                                                                                                                                                                      | db.cpp:16-53, 85-95                             | lazy/once DDL init                                                                 |
| 26   | FS_BuildOSPath 0x142246220                                                                                                                                                                        | patches.cpp:504-519                             | path normalisation                                                                 |
| 27   | DB_LoadXFile (server)                                                                                                                                                                             | dedicated/sound.cpp:28-41, 183-184              | block on sound banks after level load (flags & 0x1000C00)                          |
| 28   | R_Stream_ClearTechniqueSetShaders (server)                                                                                                                                                        | dedicated/networking.cpp:244-266                | stubbed                                                                            |
| 29   | DB_FindXAssetHeader 0x141420EF0                                                                                                                                                                   | script.cpp:1135-1167, 1482-1483                 | SPT/RawFile override + dump                                                        |
| 30   | Lua_CoD_FFReader 0x141F027E0                                                                                                                                                                      | ui_scripting.cpp:2175-2201, 2561                | swap in override buffer                                                            |
| 31   | Lua_CoD_GetRawFile 0x141F02860                                                                                                                                                                    | ui_scripting.cpp:1214-1247, 2579                | serve local ui_scripts for require                                                 |
| 32   | hks_load call (select 0x141D3CD0A)                                                                                                                                                                | ui_scripting.cpp:1199-1212, 2563-2564           | load raw script from disk                                                          |
| 33   | package.require (select 0x141D1C6C0)                                                                                                                                                              | ui_scripting.cpp:2569-2571                      | track require root                                                                 |
| 34   | SEH_StringEd_GetString (select 0x14221CBC0)                                                                                                                                                       | localized_strings.cpp:18-28, 42-43              | localized override map                                                             |
| 35   | Lua Mods_SetMod 0x141FDCEB0                                                                                                                                                                       | ui_scripting.cpp:2298-2313, 2553-2555           | immediate mod load                                                                 |
| 36   | LobbyVM_CallFunc                                                                                                                                                                                  | ui_scripting.cpp:2317-2521, 2556                | table of handlers (all nullptr) -> pass-through                                    |
| 37   | Com_GetBuildInt/StringField; load_dll                                                                                                                                                             | ui_scripting.cpp:2548-2552, 1758, 2565          | spoof build info; blacklist DLLs from Lua                                          |
| 38   | Scr_BeginLoadScripts call 0x141AA255F                                                                                                                                                             | script.cpp:1250-1266, 1489-1490                 | inject scripts/ + custom_scripts/, bind detours, scriptgdb.zip                     |
| 39   | CG_TestServerScriptChecksum                                                                                                                                                                       | script.cpp:1205, 1493-1495                      | always 1                                                                           |
| 40   | Scr_Error (flag `-log-script-errors`)                                                                                                                                                             | script.cpp:1379-1421, 1497-1501                 | callstack logging                                                                  |
| 41   | LoadScriptGDB(2), Scr_FindObjFileInfo, Scr_GetFileAndLineNum, ReportObjLinkError(2)                                                                                                               | script.cpp:1503-1516 -> impl/scr/gdb.cpp        | GDB from scriptgdb.zip and `<mods/usermaps>/<dir>/zone_source/all/scriptgdb/*.gdb` |
| 42   | Hunk_UserFree                                                                                                                                                                                     | script.cpp:1424-1464, 1504                      | don't free our script memory                                                       |
| 43   | VM opcode table                                                                                                                                                                                   | gsc/vm.cpp:119-154, 217-348                     | GetTime reimpl; detour redirect; debug flushes                                     |
| 44   | Scr_/CScr_ GetFunction/GetMethod + reverse                                                                                                                                                        | gsc_funcs.cpp:1410-1515, 1602-1626              | custom builtins                                                                    |
| 45   | G_InitGame / G_ShutdownGame call sites                                                                                                                                                            | game_event.cpp:16-34, 56-59                     | `on_g_init_game` / `on_g_shutdown_game`                                            |
| 46   | `ScrVar_*` / MT_Free / Scr_IsTrue                                                                                                                                                                 | scrvar.cpp:19-161                               | VM safety (incl. usermap flagsys bool fix)                                         |

**Not hooked anywhere:** DB_LinkXAssetEntry, DB_AddXAsset, DB_UnloadXZone
(called directly by workshop.cpp:190), FS_AddGameDirectory, FS_Startup,
FS_BuildOSPathForThread, Scr_LoadScript (called, not hooked), `UGC_ModChange_*`.

## D. "Asset override" semantics

- **Types:** RAWFILE and SCRIPTPARSETREE only (docs/asset-override.md:7-8;
  script.cpp:1143-1154).
- **Lookup:** exact, case-sensitive name before the engine (`get_loaded_script`
  and `get_loaded_rawfile` use `if_contains(name)`). Lookups for detours use
  case-insensitive and suffix matching (`get_linked_obj`, script.cpp:176-268).
- **Roots:** `%LOCALAPPDATA%/boiii/data` (`game.cpp:30-45`) and `<game>/boiii`.
  The `data/` tree is also cleaned by the updater (file_updater.cpp:674-712).
- **Rawfile trees:** listed in the facts; script.cpp:1070-1116.
  - Map variant: `<mapname>/<tree>` (map name taken from the `mapname` dvar at
    load time).
  - Mod variant: `<activeModPublisherId>/<tree>`.
  - The prefix is stripped, so the asset name is the path relative to the tree
    root.
- **Rawfile precedence:**
  - Appdata wins over the game folder within one tree (script.cpp:1060-1068).
  - Across global, map and mod trees the result is a nondeterministic race
    (`std::execution::par`, script.cpp:1046-1047, 1118-1121).
  - Inserted entries are never replaced and never freed (script.cpp:895-930; not
    cleared in 1169-1179).
  - The loader runs only from the DB_AssetPoolInit detour.
- **Lua:** `.lua` sources are compiled with HKSC (strip all, ignore debug) to
  bytecode with magic `1B 4C 75 61 51 0E 01 04` (script.cpp:932-1003).
- **Script override precedence:** first occurrence of a lower-cased name wins.
  Order: shared, then mode, then map, then mode/map, then mod, then root;
  appdata before the game folder (script.cpp:711-717, 828-865).
- **Localize:** programmatic only (localized_strings.cpp). No StringTable, KVP,
  ScriptBundle, weapon or other overrides or merges exist.
- **Asset pool config** (docs/asset-pool.md): the first `assetlimits.txt` or
  `assetpools.txt` in the item's zone tree. Lines are either `<type> <size>` or
  the T7Recharged form `<type> <name> <default> <size>`, in decimal or 0x hex.

## E. GSC/CSC pipeline details

- **Source handling** (script.cpp:456-602):
  - A file starting with the T7 magic (`80 47 53 43 0D 0A 00 1C`) is loaded
    as-is.
  - Otherwise, for `.gsc`/`.csc`: strip devblocks, compile, then record the
    hash-to-name+line table, the source text, the GDB and any replacefunc
    entries. Debug builds also write `.gscc` and `.gdb` next to the source.
- **Compiler search path for #insert** (gsc_compiler.cpp:29-84): next to the
  source and up to 10 parent directories, then `%LOCALAPPDATA%/boiii/data`,
  `data/scripts`, `<game>/boiii`, `<game>/boiii/scripts`.
- **Hashing:** `gsc_hash` = fnv1a, or a raw literal
  `hash_/var_/function_/namespace_…` (gsc.hpp:34-86).
- **Link:**
  - `custom_scripts`: `Scr_LoadScript(inst, "custom_scripts/…")`. The engine
    looks up the SPT `custom_scripts/….gsc` and the detour serves it.
  - `scripts`: served when the engine itself loads the stock script.
  - Custom scripts link right after Scr_BeginLoadScripts, before stock
    gametype/map scripts.
- **autoexe<local scratch copy, not kept>** EXPORT_AUTOEXEC=2 / PRIVATE=4
  (gsc_emitter.hpp:23-27).
  - Generated autoexecs: `__init__sytem__` from REGISTER_SYSTEM
    (gsc_compiler.cpp:522-530) and `__codex_addcommand_dispatch_bootstrap`
    (739-743).
  - Execution belongs to the engine. Fork commit d34d95d5 located an autoexec
    queue head and runner, and a Scr_EndLoadScripts wrapper, on the old exe.
- **Detours:** `function_replacements[targetCodeAddr] = replacementCodeAddr`
  (gsc_funcs.cpp:40, 500, 1541).
  - Redirect happens in `try_redirect` when `fs->pos - sizeof(OP_TYPE)` is a key
    and `inst == SERVER` (vm.cpp:94-107).
  - Bound for compile-time detours via
    `resolve_export_address_internal(script, fnv1a(lowercase fn), params)`
    (script.cpp:277-311).
  - `getfunction(script, fn)` returns the raw export address as an int
    (gsc_funcs.cpp:1165-1174).
- **Lifetime:**
  - Compiled custom and override scripts are freed at G_ShutdownGame
    (script.cpp:1169-1179, 1486).
  - Detours are cleared at G_InitGame and G_ShutdownGame
    (gsc_funcs.cpp:1798-1811).
  - `<game>/zone/scriptgdb.zip` is rewritten on every in-game load
    (script.cpp:1181-1203).
- **Other runtime pieces:**
  - Global hash-name table: `data/lookup_tables/hash_names.txt`
    (script.cpp:1211-1248).
  - File helpers: rooted at `boiii/scriptdata`.
  - Commands: `addcommand`/`getcommand` (server) plus a generated dispatcher.

## F. Struct layouts (as defined upstream)

**XZoneInfo** (pack 1, 0x28) `structs/db/xzone.hpp:61-69`
`0x00 const char* name | 0x08 i32 allocFlags | 0x0C i32 freeFlags | 0x10 i32 allocSlot | 0x14 i32 freeSlot | 0x18 XZoneBuffer{0x18 const void* data, 0x20 size_t dataSize}`
(the in-memory `fileBuffer` path exists in the struct)

**XZoneName** (pack 1, 0x60) `:73-85`
`0x00 char name[64] | 0x40 i32 flags | 0x44 i32 slot | 0x48 ZoneType zoneType | 0x4C pad4 | 0x50 size_t size | 0x58 XZoneState state | 0x5C bool streamPreloaded | 0x5D pad3`

- XZoneState values: -1 UNLOADING, 0 EMPTY, 1 LOADING, 2 LOADED, 3 COMPLETE, 4
  FAILED (`:19-26`).
- `g_zoneNames` = ZonePool<XZoneName> with 64 entries; MAX_ZONE_COUNT is 63
  (`:14-18`).

**XZoneInfoInternal** (0x58) `:37-53`
`0x00 char name[64] | 0x40 int allocFlags | 0x44 int freeFlags | 0x48 XZoneInfoInternal* next (likely) | 0x50 XZoneInfoInternal* prevMaybe (unknown)`

**XZone** (pack 1, 0xB0) `:90-98`
`0x00 u16 index | 0x02 pad2 | 0x04 int flags | 0x08 int slot | 0x0C pad4 | 0x10 XBlock blocks[10]`
XBlock (0x10) is `{u8* data; u64 size}` (`structs/db/core.hpp:11-15`).

**ZoneType** `structs/core.hpp:544-560`: OFFICIAL 0, MOD 1, USERMAP 2.
`dirname()` gives "mods" or "usermaps" (`:575-584`).

**XAsset** (pack 1, 0x10) `structs/db/xasset/core.hpp:940-945`:
`0x00 XAssetType type | 0x04 pad | 0x08 XAssetHeader header`.

- XAssetHeader is a union of pointers (`:831-936`). Typed members: named,
  physPreset, physConstraints, destructibleDef, model, modelMesh, techniqueSet,
  image, sound, soundPatch, clipMap, comWorld, gameWorld, gfxWorld, lightDef,
  font, fontIcon, localize, `weapon(WeaponVariantDef*)`,
  customizationTable(+FEImages/Color), sndDriverGlobals, rawfile, stringTable,
  ddl, scriptParseTree, keyValuePairs, vehicleDef/FxDef/SoundDef, xModelAlias,
  ttfDef, bgCache, mapTable, data.
- Material, fx, attachment, weaponCamo, scriptBundle, structuredTable and others
  are commented out (undefined).

**XAssetEntry** (pack 1, 0x20) `:968-977`:
`0x00 XAsset asset | 0x10 u8 zoneIndex | 0x11 bool inuse | 0x12 pad2 | 0x14 u32 nextHash | 0x18 u32 nextType | 0x1C u32 nextOverride`

**XAssetList** (0x20) `:959-965`:
`0x00 ScriptStringList{i32 count; pad; const char** strings} | 0x10 i32 assetCount | 0x14 pad | 0x18 XAsset* assets`

**XAssetType** `:108-222`: values 0x00 PHYSPRESET through 0x66 STREAMER_HINT,
COUNT 0x67. RAWFILE 0x2F, STRINGTABLE 0x30, SCRIPTPARSETREE 0x36, WEAPON 0x17,
LOCALIZE_ENTRY 0x16, KEYVALUEPAIRS 0x37, SCRIPTBUNDLE 0x45. DEPEND=0x68
duplicates STRING (`:220`).

**XAssetPool** (pack 1, 0x20) `structs/db/xasset/pool.hpp:33-61`:
`0x00 void* pool | 0x08 u32 itemSize | 0x0C i32 itemCount | 0x10 bool isSingleton | 0x11 pad3 | 0x14 i32 itemAllocCount | 0x18 AssetLink* freeHead`

- `XAssetPools` = union{XAssetPool pools[0x67]; TypedXAssetPools typed}
  (`:109-233`). `typed.navmesh` is typed as GfxUmbraTome (`:210`).
- `XAssetEntryPool` = XAssetEntryPoolEntry[0x263FF], where the entry is
  `union{XAssetEntry entry; next*}` (`:97-107`).
- `db_hashTable` = u32[0x26400] (symbols/db/core.hpp:10; suspicious legacy
  address).

**RawFile** (pack 1, 0x18) `:244-257`:
`0x00 const char* name | 0x08 size_t len | 0x10 u8* buffer`

**LocalizeEntry** (pack 1, 0x10) `:1023-1027`:
`0x00 const char* value | 0x08 const char* name` (value comes first)

**KeyValuePairs** (0x18) `:818-829`:
`0x00 name | 0x08 i32 numVariables | 0x10 KeyValuePair*{i32 keyHash; const char* value}`

**StringTable** (0x20) `structs/ddl.hpp:101-108`:
`0x00 const char* name | 0x08 i32 columnCount | 0x0C i32 rowCount | 0x10 StringTableCell* values | 0x18 i16* cellIndex`

- StringTableCell (pack 1, 0x10) `:95-99`:
  `0x00 const char* string | 0x08 i32 hash | 0x0C pad4`.
- No StringTable lookup, hash or sort functions are declared upstream.

**ScriptParseTree** (pack 1, 0x18) `structs/scr/core.hpp:1911-1916`:
`0x00 const char* name | 0x08 size_t len | 0x10 GSC_OBJ* buffer`

**GSC_OBJ** (pack 1, 0x48) `:1748-1909`, magic `80 47 53 43 0D 0A 00 1C`:
`0x08 source_crc | 0x0C include_offset | 0x10 animtree_offset | 0x14 cseg_offset | 0x18 stringtablefixup_offset | 0x1C devblock_stringtablefixup_offset | 0x20 exports_offset | 0x24 imports_offset | 0x28 fixup_offset | 0x2C profile_offset | 0x30 cseg_size | 0x34 name(offset) | 0x38 u16 stringtablefixup_count | 0x3A u16 exports_count | 0x3C u16 imports_count | 0x3E u16 fixup_count | 0x40 u16 profile_count | 0x42 u16 devblock_stringtablefixup_count | 0x44 u8 include_count | 0x45 u8 animtree_count | 0x46 u8 flags | 0x47 pad`

- **GSC_EXPORT_ITEM** (pack 1, 0x14) `:1703-1717`:
  `0x00 u32 checksum(crc32) | 0x04 u32 address(cseg off) | 0x08 u32 name(fnv1a) | 0x0C u32 name_space | 0x10 u8 param_count | 0x11 u8 flags | 0x12 pad2`
- **GSC_IMPORT_ITEM** `:1719-1739`:
  `u32 name | u32 name_space | u16 num_address | u8 param_count | u8 flags`,
  followed by `u32 addresses[num_address]`.
  - Import flags (gsc_emitter.hpp): GETFUNCTION 1, CALL 2, THREAD 3, METHOD 4,
    METHOD_THREAD 5.
  - Combined with EXTERNAL 0x00, EXTERNAL_DEV 0x10, LOCAL 0x20, LOCAL_DEV 0x30.
- **GSC_GDB** (0x28) `:1937-2050`, magic `80 47 44 42 0D 0A 00 13`.
- **objFileInfo_t** (pack 1, 0x50) `:2090-2095`:
  `GSC_OBJ* activeVersion | GSC_OBJ* baselineVersion | debugFileInfo_t debugInfo (0x40: filename, startAddr, endAddr, lineStartAddr, lineStartAddrCount, source, sourceLen, gdb; :2053-2079)`.
  ObjFileInfoPool = ScrPool<array<objFileInfo_t,500>> (`:2097`).

**WorkshopData** (0x4C8) `structs/ugc.hpp:14-55`:
`0x000 char title[100] | 0x064 char internalName[32] | 0x084 char publisherId[32] | 0x0A4 char description[256] | 0x1A4 char contentPathToZoneFiles[260] | 0x2A8 char absolutePathContentDirectory[260] | 0x3AC char absolutePathZoneFiles[260] | 0x4B0 u32 version(=1) | 0x4B4 pad | 0x4B8 u64 publisherIdInteger | 0x4C0 u32 publisherIdHash(djb2 seed 0x1505 x33, ugc.cpp:81-87) | 0x4C4 ZoneType type`

- **ActiveMod** (0x4D0) `:64-68`: WorkshopData, then
  `0x4C8 ModLoadState loadState` (0 IDLE, 1 LOADING, 2 COMPLETE).
- **Pools** `:76-208`: `{u32 count; pad; WorkshopData data[N]}`. The client
  variant adds `LocalClientPool<UIModelIndex> listModels`. Builtin N=128
  (0x26408 / 0x26410); extended N=8192 (DLL statics `symbols/ugc.cpp:9-10`).

**MapTable** (pack 1, 0x28) `structs/db/xasset/maptable.hpp:59-69`:
`name | u32 mapCount | pad | MapTableEntry* maps | eModes sessionMode | CampaignMode | dlcIndex_t | pad`.
MapTableEntry is at `:17-57` (name, mapName, previewImage, loadingImage,
mapVersion, kvpCount/kvpItems, uniqueID…).

**WeaponVariantDef** (pack 1, 0x300 "Verified") `structs/weapon.hpp:2324-2424`:
`0x00 szInternalName | 0x08 szModeIndependentName | 0x10 int sessionMode | 0x14 int iVariantCount | 0x18 WeaponDef* weapDef | 0x20 szDisplayName | 0x28 szAltWeaponName | 0x30 szAttachmentUnique | 0x38 attachments | 0x40 attachmentUniques | 0x48 szXAnims | 0x50 hideTags | 0x58 attachViewModel | 0x60 attachWorldModel | 0x68/0x70 view/world tags | 0x78 4x float[15] offsets/rotations | 0x168 stowedModelOffsets | 0x174 stowedModelRotations | 0x180 altWeaponIndex | 0x188 iAttachments | 0x190 bIgnoreAttachments | 0x194 iClipSize …`
(reload/ADS timings, ammo/clip names+indices, zoom, overlay/dpad, IK follow).

- **WeaponDef** `:1024+` (szInternalName, szModeIndependentName, sessionMode,
  iVariantCount, szDisplayName, …, viewModel…). It has no ASSERT_SIZE; the DB
  size table gives 0x15F0 (impl/db/db.cpp:91-92), and the WEAPONDEF pool default
  is 0.
- **WeaponCamo** (pack 1, 0x18) `:470-476`:
  `name | WeaponCamoMaterialSet* camoMaterials | u32 numCamoMaterials | pad`.

**XFile header** `structs/db/xfile.hpp:29-49`: magic, version, server,
compression, platform, encrypted, timestamp, changelist, archiveChecksum[4],
builder[32], metaVersion, mergeFastfile[64], size, externalSize,
memMappedOffset, blockSize[10], fastfileName[64], signature[256], aesIV[16]. The
auth and crypto fields are fenced off.

**DB_LoadData** (0xA88 "Verified") `structs/db/load.hpp:179-207`.

## G. Asset pools

- **Defaults:** identical tables in `impl/db/db.cpp:277-382` and
  `asset_limits.cpp:75-193`. Examples: RAWFILE 0x1388, STRINGTABLE 0xDC,
  SCRIPTPARSETREE 0x47E, WEAPON 0x600, LOCALIZE 0x6400, XMODEL 0x2C00, IMAGE
  0xC000, worlds 2.
- **Sources:**
  1. `<game>/boiii_players/user/launcher_settings.json`: `asset_limits_enabled`,
     `disable_asset_pools`, `ap_<pool>` (asset_limits.cpp:195-259).
  2. Mod/usermap `assetlimits.txt` (ugc.cpp:259-274, 316, 547).
  3. Auto-doubling on exhaustion (db.cpp:500-505).
- **Rules:** a pool grows only if `!isSingleton && itemCount < new_size`
  (db.cpp:397-398).
- **Hazards:** see the risks. The docs say GFX/COM/Game world, IMAGE and
  XMODELMESH pools are unsafe or ineffective to grow
  (docs/asset-pool.md:205-222; asset_limits.cpp:81-96).

## H. fastdl (usermaps only)

1. **URI:** `<sv_wwwBaseURL>/usermaps/<mapname>`. `manifest.json` =
   `[[name,size,sha1],…]` (fastdl.cpp:120-180).
2. **Prompt:** a MessageBox shows the total GB (fastdl.cpp:395-412).
3. **Downloads:** files are fetched serially with curl (`FOLLOWLOCATION`, verify
   peer/host, no timeout) to `./usermaps/<map>/<name>`. Each file is checked by
   size and SHA1 and deleted on failure (fastdl.cpp:182-329).
4. **workshop.json:** downloaded last, in memory, with size and SHA1 checks
   (fastdl.cpp:280-309). The map only becomes visible after that.
5. **Rescan:** UGC_LoadPools_Impl runs on main (workshop.cpp:807-809).
6. **Skip rule:** files of map types are skipped when the size matches
   (fastdl.cpp:331-354).
7. **Path guard:** relative paths without `..` (fastdl.cpp:88-109).
8. **Dead code:** `out_relative` is computed but unused (fastdl.cpp:194-195,
   301-302).

## I. Fork PoC empirical notes (old exe; refs/heads/main)

- **Commit 88912f57:**
  - Mod tier allocFlags 0x40 (core_mod) and 0x8000040 (en_core_mod).
  - Other tiers: core_pre_gfx 0x1, core_gfx 0x2, core_post_gfx 0x4, core_ui 0x8,
    core_common 0x20, core_ffotd 0x2000, specialists 0x400000, frontend
    0x800000. The patch bit is 0x10000000 and the language-pair bit is
    0x8000000.
  - DB_LoadXAssets auto-prefixes `en_`.
  - FS search-path list head on the legacy exe is 0x157A65308.
  - `inner_loadMod` is UGC_LoadMod, which calls FS_ShutdownSearchPaths when
    reloadFS is set.
- **Fork patches.cpp:285-307:** the engine raises "PMem_Free … not at the top of
  the stack" when same-named zones (Solo slot 16 and DS4C slot 23 "core_mod")
  are unloaded, and "Could not load default asset" with stale cross-mod refs.
- **Address consistency:** the fork's addresses match upstream's legacy column
  (e.g. DB_LoadXAssets 0x1414236A0, UGC_LoadMod 0x1420D6B50, FS_AddGameDirectory
  0x1422A28D0). The equivalents on the current exe are in the table in A.

## J. Latent bugs and oddities seen (not fixed; read-only)

- `supplement_ugc_from_workshop` checks `usermapsPool.count` for both pool types
  (workshop.cpp:315).
- No de-dupe between Steam-manifest and folder-scanned items
  (workshop.cpp:349-414).
- `workshop.hpp:30` declares
  `mod_load_requires_fs_reinitialization(std::string&)`, but the cpp defines the
  `const&` overload (workshop.cpp:970).
- The Mods_SetMod detour defers a raw Lua `const char*`
  (ui_scripting.cpp:2307-2311).
- `lua_reload_mod` builds `<absolutePathContentDirectory>/mods/<id>`, which is
  wrong for Workshop items (ui_scripting.cpp:2047-2067).
- `workshop_id.cpp` detaches its thread, then joins only if joinable (203-219).
  It also rewrites the server cfg and pops a MessageBox on dedicated ZM.
- Two unused symbol entries have suspicious addresses: `db_hashTable` and
  `DB_AssetPoolRelease`. `DB_GetAssetEntryPoolEntryByName` walks the wrong link.
- Every UGC_LoadModByPublisherId_Impl call does a full pool rescan, including
  the "usermaps" load on every usermap map (ugc.cpp:286).

## Design implications

- Build a pattern-scanned symbol table for at least: DB_LoadXAssets,
  DB_LinkXAssetEntry, DB_UnloadXZone, DB_FindXAssetHeader, DB_CheckModXFile,
  FS_BuildOSPathForThread, FS_AddGameDirectory, FS_Startup, UGC_LoadMod,
  UGC_LoadModByPublisherId, UGC_SetActiveUsermap, Scr_LoadScript,
  Scr_BeginLoadScripts (call site), Scr_GetFunction/Method, `CScr_*`,
  Mods_Lists_GetInfoEntries_Slice, Lua `Mods_*` APIs, and the globals
  g_zoneNames/g_zoneCount/s_assetPools/g_assetEntryPool/active_mod/active_usermap/fs_game.
  Use a new scanner module, not arxan.cpp. Require exactly one match and fail
  closed. Keep the upstream triples only as a cross-check.
- Introduce one hook-owner per engine function: a dispatcher with ordered
  callbacks. Migrate upstream's existing detours on DB_FindXAssetHeader,
  DB_LoadXAssets, the `UGC_*` family and `Scr_Get*` into it, so Xerces modules
  never call MinHook directly on an already-hooked address.
- Define a mod manifest (id, version, content hash, declared provides/overrides,
  priority, dependencies, script entry points) and a resolver that yields a
  deterministic load order: explicit priority, then a stable tiebreak. Sort
  every directory enumeration.
- Asset ownership map: hook DB_LinkXAssetEntry (current-client symbol
  0x1414228E0) and record (type, name) -> zoneIndex -> mod. Use g_zoneNames to
  map zone slots to mods. This gives conflict detection and reports before and
  after load. The struct fields (zoneIndex, nextOverride) already exist.
- Generalise DB_FindXAssetHeader_TryOverride into a typed override/merge
  registry scoped to the active mod set and cleared on set change. Cover
  RAWFILE, SCRIPTPARSETREE, LOCALIZE, and STRINGTABLE merges (row/key merge
  producing a new StringTable with rebuilt cell hash and cellIndex; the T7 hash
  and sort routine must be found by pattern). Use explicit priority for
  exclusive types, and replace the never-cleared loaded_rawfiles design.
- Choose and prototype a zone strategy on build 24784313 before committing. (a)
  Primary mod in the engine's mod tier, plus secondary mods' fastfiles as
  uniquely named zones resolved to their real paths via the zone path builder
  (FS_BuildOSPathForThread/DB_CheckModXFile multi-root), loaded in one batch and
  unloaded together to respect PMem LIFO and en_ pairing. (b) Script-only and
  override-only mods (no zones) through per-mod script trees. (c) Pre-merged
  'pack' build steps. Any approach must load through the engine's normal
  verified path and leave fastfile authentication alone.
- Replace function_replacements with a chain registry: target -> ordered list of
  (mod, replacement). Provide call-original via a generated trampoline export or
  a per-call bypass flag. Support CSC (remove the inst == SERVER restriction
  where safe), namespace hooks per mod, and remove the global clearreplacefuncs
  semantics. Bind after all mods' scripts are known, not only at the first
  Scr_BeginLoadScripts.
- Load each mod's GSC/CSC from its own folder (custom_scripts/<modid>-like) in
  resolved order with sorted enumeration. Make precedence explicit (more
  specific beats generic) instead of first-wins. Budget against the 500
  objFileInfo limit and the SCRIPTPARSETREE pool.
- Asset pools: before any zone loads, compute max(user config, all enabled mods'
  assetlimits). Apply them once at DB_AssetPoolInit. Never free a pool that may
  hold live assets; reserve instead of re-growing. Keep world/IMAGE/XMODELMESH
  at their defaults.
- Network: advertise the ordered mod set with versions and hashes instead of a
  single modId. Re-implement mismatch detection at the Xerces protocol level
  (engine anti-tamper untouched). Extend fastdl to mods with signed manifests
  (e.g. ed25519), SHA-256, an extension allowlist (no .dll), size caps and a
  sandboxed destination.
- Workshop: drive subscribe/download/install through the official Steam client
  path (ISteamUGC via the engine's Lua Mods_SubscribeUGC/InstalledUGC or the
  existing UGC_LoadManifest data, consuming steam_proxy read-only). Drop
  steamcmd anonymous downloads and HTML scraping.
- In-game mods menu: reuse the existing data plumbing
  (Mods_Lists_GetInfoEntries_Slice, UI models ModsLoadEntry.update /
  UsermapsEntries.update / ModsGlobal.update, the Mods_SetMod detour) or a
  ui_scripts LUI menu. Route selection to Xerces 'profile/pack' activation
  instead of single-mod UGC_LoadMod.
- Move user and mod content out of updater-managed trees and centralise path
  roots (replace the 46 'boiii' literals). Decide deliberately whether to keep
  or replace the IsBOIII/KNOWN_MALICIOUS_MODS identity behaviour.
- Use the existing lifecycle hooks (com::on_level_load/on_level_unload,
  game_event on_g_init_game/on_g_shutdown_game, the CL_SetupForNewServerMap
  detour) as Xerces loader phases, and add explicit before/after-zone-load and
  script-bind phases.

## Risks

- The engine is built around one active mod: a single active_mod/ActiveMod, a
  single fs_game, DB_CheckModXFile_Impl checks only the active mod directory,
  and ZoneType has only MOD/USERMAP with no mod id. Any additive design works
  against this, and swapping mods triggers a full FS reinit.
- Every BO3 mod ships zones with the same names (core_mod/en_core_mod,
  zm_mod/mp_mod/cp_mod) in the same tier. Per fork PoC evidence (old exe),
  loading a second mod's zones hits the en_ auto-prefix and same-tier pairing
  ('Could not find zone en_X') or the LIFO PMem error ('PMem_Free … not at the
  top of the stack') on map transitions.
- The zone table caps at 64 slots, with the engine erroring at 64
  (structs/db/xzone.hpp:14-16). Stock zones plus several mods' zones (each with
  an en_ pair) can exhaust it.
- Asset name collisions across mods resolve as last-loaded-wins through the
  engine's override chain (XAssetEntry.nextOverride). Unloading one mod's zone
  can leave other mods' cross-references stale ('Could not load default asset',
  fork evidence).
- Pool reallocation hazards (impl/db/db.cpp:411-451): only the first
  itemAllocCount slots are copied; the previous heap-extended pool is free()d
  while live asset headers may still point into it (use-after-free on the second
  growth); release paths are not locked; world/IMAGE/XMODELMESH pools are unsafe
  to grow, but auto-doubling can still trigger during zone load.
- Rawfile override precedence across global, map and mod trees is a
  nondeterministic race (std::execution::par plus insert-if-absent). Overrides
  are never cleared, so one mod's overrides leak into later sessions with other
  mods. They load only from the DB_AssetPoolInit detour, and it is unknown
  whether that re-runs on mod or map change.
- Script override precedence is first-wins, and a mode-generic override
  (scripts/zm/x.gsc) beats a map- or mod-specific one. File enumeration
  (utils::io::list_files) is unsorted, so link and autoexec order depend on the
  filesystem.
- GSC detours: one map slot per target (a second mod silently overwrites the
  first), no call-original or chaining, server VM only (no CSC),
  clearreplacefuncs wipes every mod's hooks, unresolved detours wait until the
  next map, and the map is cleared at G_InitGame (ordering versus script load
  not verified).
- Each VM is limited to 500 linked script objects, and the SCRIPTPARSETREE pool
  defaults to 0x47E. Several script-heavy mods plus custom_scripts can exceed
  these.
- MinHook allows one detour per address. Upstream already owns
  DB_FindXAssetHeader, DB_LoadXAssets, DB_CheckModXFile, the `UGC_*` family,
  `Scr_Get*` and others, so Xerces features must be layered through a central
  dispatcher or hook creation will throw.
- Every address is hard-coded per build (3,473 symbols, 136 inline sites,
  20-entry call-site arrays, function+0x81 offsets). The next Steam update
  breaks all loader hooks unless they are pattern-scanned.
- Mod identity is one string with version always 1, the client/server GSC
  checksum test is forced to pass, and the BG cache mismatch error is
  suppressed. Client/server mod-set or version mismatches go undetected and can
  desync or crash.
- The mod pools can contain duplicates (Steam manifest plus content-folder
  scan), and lookups return the first match, so metadata and paths can be
  inconsistent.
- fastdl: the manifest is unsigned and SHA1 comes from the same server; any
  extension is downloaded (including .dll); map-type files that exist with the
  right size are never re-hashed; usermaps only. A malicious server can push
  arbitrary fastfiles, which are an engine parser attack surface.
- The updater deletes unlisted files under %LOCALAPPDATA%/boiii/data, the
  documented home for scripts/, custom_scripts/ and ui_scripts/.
- Workshop acquisition uses steamcmd anonymous downloads plus steamcommunity
  HTML scraping. This conflicts with the 'official mechanisms only' requirement.
- Custom CSC (and CSC overrides) load only from the server's script-load call
  site. A client joining a remote dedicated server may never load them.
- Engine 'mod loaded' checks are forced false (20 call sites plus
  Scr_IsModLoaded). Mod-aware stock UI or logic may behave as if unmodded.
- Client-identity spoofing (Engine.IsBOIII, KNOWN_MALICIOUS_MODS, build-field
  spoof) is tied to the 'boiii' identity. Renaming to Xerces changes what mods
  detect, and 46 hard-coded 'boiii' path literals are spread across 19 files.
- The Mods_SetMod detour defers a raw Lua string pointer, which can dangle.
  wait_for_mod_load busy-waits on the calling thread.

## Open questions

- Does the engine compare XFile.fastfileName (or anything the signature covers)
  against the requested zone name? That decides whether a second mod's
  core_mod.ff can be loaded under a unique alias. It must be investigated
  without modifying any authentication path.
- Where does the current exe call DB_CheckModXFile and FS_BuildOSPathForThread,
  and what does zone_internal_id carry? Can it support per-zone mod roots for
  more than one mod?
- What are the exact T7 semantics of DB_LinkXAssetEntry(allowOverride) and the
  nextOverride chain, and how is the chain unwound on DB_UnloadXZone? This
  defines what an ownership map and a safe partial unload must do.
- When does DB_AssetPoolInit re-run (startup only, FS restart on mod load, map
  load)? It controls when rawfile overrides and pool sizes are applied.
- Does on_g_init_game (the Rope_InitRopes call site in G_InitGame) run before
  the server's script-load call site? If it runs after, compile-time detours are
  cleared right after binding.
- Do custom CSC scripts and CSC overrides load on a client connected to a remote
  dedicated server, given the injection point is in the server-side game-script
  loader? Is Scr_LoadScript(CLIENT) safe at that moment on a listen server?
- Do the Steam-manifest and Workshop-folder scans actually produce duplicate
  pool entries when Steam is running with subscriptions?
- Which engine code still reads builtinModsPool/builtinUsermapsPool directly
  (not covered by the 14 UGC detours)?
- Does UGC_LoadMod finish asynchronously (UGC_ModLoad_TryComplete exists)? Is
  wait_for_mod_load's busy-wait safe on the thread it runs on?
- Are the fork's zone-tier observations (mod tier 0x40, en_ bit 0x8000000, en_
  auto-prefix, PMem LIFO) identical on build 24784313 (CheckSum 0x6531394)?
- What are the StringTable cell hash function and the cellIndex layout and sort
  order on T7? Both are required for row/key merges.
- Does auto-doubling ever fire for IMAGE/XMODELMESH/world pools during a heavy
  multi-mod load, and what breaks if it does?
- What stock UI or logic changes because the engine is told no mod is loaded
  (client_patches.cpp:81-108, 473-480), and does a Xerces mods menu need the
  real state?

## Fenced (do not modify)

- src/client/component/arxan.cpp — Arxan anti-tamper handling. It is also the
  only user of the utils::hook::signature scanner. Treat it as a fenced module;
  Xerces pattern-scanning must live elsewhere and must not reuse or modify its
  integrity logic.
- src/client/component/integrity.hpp — table of Arxan integrity-check block
  addresses (intact_integrity_check_blocks).
- src/client/game/symbol.hpp:64-73 (symbol::call_safe) together with
  arxan::detail::callstack_proxy_addr / set_address_to_call — the Arxan
  call-stack proxy used when calling engine functions. Treat it as anti-tamper
  plumbing; use as-is and don't modify.
- src/client/component/steam_proxy.cpp/.hpp — proxy to the real Steam client. It
  carries subscription data (Workshop enumeration flows through it into
  src/client/steam/interfaces/ugc.cpp:247-300) and the ownership/licensing
  surface. Consume its read-only subscription API only.
- src/client/steam/interfaces/apps.cpp/.hpp, user.cpp/.hpp, game_server.cpp/.hpp
  — Steam interfaces containing app ownership, license and auth-session APIs
  (grep hits for ownership/license/auth session).
- src/client/game/demonware/servers/auth3_server.cpp — Demonware auth-ticket
  handling (ownership-adjacent).
- src/client/component/dedicated/networking.cpp:278-285 — NOPs the engine's
  deliberate crash on fastfile 'authload' failure (0x1401A18E1, 0x1401A1B5D
  server). This is the fastfile authentication path; do not extend or rely on
  it.
- Fastfile authentication/encryption data: structs/db/xfile.hpp:29-49
  (encrypted, signature[256], aesIV[16]) and structs/db/load.hpp
  (DBDecompressCmd_t.isEncrypted, DB_LoadData.numBlocksDecrypted). Additive
  loading must go through the engine's normal zone loader with unmodified
  verification.
