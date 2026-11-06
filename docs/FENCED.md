# Fenced code

These parts of the inherited code handle Steam ownership or licences, auth
tickets, anti-tamper (Arxan), Treyarch anti-cheat (TAC) workarounds, or fastfile
authentication. Xerces commits must not modify them, extend them, port them to
signature scanning, or route new features through them. Upstream merges may
change them; that is the only way they change.

When a Xerces feature seems to need one of these, stop and ask instead of
working around the fence.

| Area                    | Path                                                                                                                      | What is fenced                                                                                      |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| Anti-tamper             | `src/client/component/arxan.cpp`                                                                                          | whole file, including its pattern scans and the `-dump` writer                                      |
| Anti-tamper             | `src/client/component/integrity.hpp`                                                                                      | whole file (integrity-check block table)                                                            |
| Anti-tamper             | `src/common/utils/hardware_breakpoint.*`                                                                                  | whole files; do not reuse for Xerces features                                                       |
| Anti-tamper             | `src/client/game/symbol.hpp`, `src/client/game/game.hpp`                                                                  | `symbol::call_safe` and the `arxan::detail` call-stack proxy (use as is)                            |
| Anti-tamper             | `src/client/component/dump.cpp`, `src/common/utils/flags.cpp` `-dump`                                                     | whole file and the flag                                                                             |
| Anti-tamper             | `src/client/component/client_patches.cpp`, `src/client/component/patches.cpp`, `src/client/component/dedicated/sound.cpp` | the Arxan workaround blocks only                                                                    |
| Steam ownership         | `src/client/component/steam_proxy.*`                                                                                      | whole files                                                                                         |
| Steam ownership         | `src/client/steam/**`                                                                                                     | whole tree (Steam API emulation, licence and ticket answers)                                        |
| Steam ownership         | `src/client/main.cpp`                                                                                                     | `patch_steam_import`, `patch_imports`, `restart_app_if_necessary_stub`, `handle_process_runner`     |
| Steam ownership         | `premake5.lua`                                                                                                            | the `--dev-build` and `--no-check` options and their defines                                        |
| Steam ownership         | `deps/SteamworkSDK`                                                                                                       | the submodule pin and its dependabot exclusion                                                      |
| Auth                    | `src/client/component/auth.cpp`                                                                                           | whole file (Steam auth-session patches, identity key derivation, XUID salts)                        |
| Auth                    | `src/client/game/symbols/live/steam/**`                                                                                   | declarations used by `auth.cpp` (reading is fine)                                                   |
| Demonware               | `src/client/game/demonware/servers/auth3_server.*`                                                                        | whole files                                                                                         |
| Demonware               | `src/client/game/demonware/services/bdAnticheat.*`                                                                        | whole files                                                                                         |
| Demonware               | `src/client/component/demonware.cpp`                                                                                      | the publisher-file signature skip                                                                   |
| Entitlement, anti-cheat | `src/client/component/live.cpp`                                                                                           | the DLC entitlement helpers, `LiveAntiCheat_ConsoleDetailsReported`, `LiveSteam_NotVacBanned` stubs |
| TAC workarounds         | `src/client/component/client_patches.cpp`                                                                                 | `store_tac_protected_allocs` and its helpers                                                        |
| TAC workarounds         | `src/client/game/impl/cg/cg.cpp`                                                                                          | the static pools replacing TAC-protected allocations                                                |
| TAC workarounds         | `src/client/component/gsc/func/**` (was `gsc_funcs.cpp`), `src/client/component/gsc/vm.cpp`                               | the re-implementations that avoid TAC-protected access                                              |
| Fastfile authentication | `src/client/component/dedicated/networking.cpp`                                                                           | the authload crash NOPs                                                                             |
| Fastfile authentication | `src/client/game/structs/db/xfile.hpp`, `src/client/game/structs/db/load.hpp`                                             | signature, encryption and decrypt fields                                                            |

Pending classification (treat as fenced until reviewed):
`src/client/game/structs/live/core.hpp` (licence verification bits),
`src/client/game/structs/dw/core.hpp` (licence error codes),
`src/client/game/structs/sys.hpp`, `src/client/game/symbols/lua/cod/api.hpp`,
`src/client/loader/**`, and the game load and TLS handling in
`src/client/main.cpp`.

Reviewed, not fenced (2026-11-05, review asked for by the user): the TLS helper,
meaning `src/tlsdll/**` (`dllmain.cpp`, `tlssup.cpp`, `resource.rc`,
`app.manifest`), the `tlsdll` project in `premake5.lua` and the `TLS_DLL`
resource that embeds `tlsdll.dll` in `boiii.exe`. It is loader plumbing. The DLL
holds one 8 KiB `thread_local` block and a TLS directory, so when Windows loads
it, Windows gives it a TLS index and every thread a block of that size.
`src/client/loader/tls.cpp` extracts and loads it, and `load_tls` in
`src/client/loader/loader.cpp` gives that index and block to the game
executable, which the client maps itself and whose TLS Windows does not set up
for it. Nothing in it checks ownership, licences or file integrity, or deals
with Arxan or TAC. Upstream added it in 2022 (e752501d "Finish process wrapper",
564e5e40 "Fix tls dll"); the later commits to these files are style, formatting
and build changes by their titles, and the comments and pragmas of `tlssup.cpp`
did not change in meaning. What was read: `dllmain.cpp`, `resource.rc` and
`app.manifest` in full; `tlssup.cpp` only its comments, pragmas and section
names (so that its rewrite stays clean-room), together with the symbols,
sections and TLS directory of the built `tlsdll.dll`; the `tlsdll` premake
project; the `git log --follow` history of these files. Also read, but not
reclassified, so they stay pending: `src/client/loader/**` (read in full;
`tls.cpp` and `load_tls` are TLS plumbing, but `component_interface.hpp` orders
the fenced Arxan and `steam_proxy` components), and in `src/client/main.cpp`
only `load_process`, `get_tls_callbacks`, `run_tls_callbacks`, `tls_runner` and
the part of `main` that loads the game.

Do not call or build on (not fenced as code, but Xerces flows never use them;
see DESIGN.md D15): upstream's Workshop and map downloaders, meaning
`workshop/steamcmd.*`, `workshop/steam_cdn.*`, the download queue in
`workshop/download.*`, `launcher/launcher_workshop.*`, `fastdl.*` (v1), and the
join-time prompts (`check_valid_mod_id`, `check_valid_usermap_id`,
`check_valid_mod_stack`) that call them. Xerces builds leave the downloader
files out (premake option `--with-upstream-downloaders`, off by default) and the
join-time prompts name what is missing instead.

Content fence: upstream's README positioning ("removes Steam ownership
verification", "No Steam ownership required") is not carried into Xerces docs,
branding or launcher text.

Enforcement: this list was re-derived on upstream `beta` f8f7b735 (2026-11-04;
`gsc_funcs.cpp` had been split into `gsc/func/`, fenced whole until reviewed).
`tools/ci/fences.json` is its machine-readable form, and
`tools/ci/check_fenced.py` (in CI) checks whole-file fences by path and partial
fences by a normalized-token hash of each fenced function, anchored by name; the
Arxan workaround blocks are anchored by their functions
(`disable_dw_userfile_writes`, `patch_fs_functions`, `enable_sound`). Partial
fences without a stable name are fenced whole there. Any non-merge commit that
changes a fenced block fails; commits that touch a file containing a fenced
block need a "fence reviewed" note in the message or a line in
`tools/ci/fence_reviews.txt`. Merge commits from upstream are exempt.
