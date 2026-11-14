# Engine reverse engineering notes

What was learned by reading the game's code (a decrypted module dump) and by watching
the running client, with how each finding was checked. Addresses are the current client
build's (`BlackOps3.exe` SHA-256 `51ca63bb...`, Steam build 24784313), at the preferred
base 0x140000000 that upstream's symbol tables use.

## How to read the game's code

- `BlackOps3.exe` on disk is encrypted: its first `.text` section has an entropy of 7.91
  bits a byte. The code is only readable from a running game: `tools/dev/re/dump-module.ps1`
  saves the decrypted image, `dump-xrefs.ps1 -Fix` writes the `.pe` copy `llvm-objdump`
  reads (DESIGN.md 4.5). The dump of 2026-11-04 (`dumps\BlackOps3-20261104-223001.bin`)
  still matches the installed game (same file hash).
- `tools/dev/re/disasm.py` disassembles a whole function at an address and names every
  call target from the symbol tables; `--calls` keeps only the calls, `--callers` lists
  every call to an address grouped by the nearest name, `--bounds` gives the function an
  address is in, `--find` searches the names. Set `XERCES_RE_OBJDUMP` to the toolchain's
  `llvm-objdump.exe`.
- Function bounds come from `.pdata`: 68,694 entries. Leaf functions (no stack frame) have
  none. The copy protection splits functions and interleaves junk bytes and jumps into
  its own sections (0x15xxxxxxx), so a range is exact for what it covers but one source
  function can be several ranges.
- A dump is taken from a client that already has its hooks in: upstream's detours show as
  a `jmp` at a function's first byte (`CL_ConnectFromLobby`, `Com_LoadLevelFastFiles`).
- Names from the symbol tables are hints: `disasm.py` gives the nearest known name at or
  below an address, which is often a different function.

## Lobbies: who hosts

- `LobbyHost_IsHost(type)` (0x141ECC700) takes this PC's own host session for that lobby
  type, `&lobbyHostData[type]` (0x1555FD410, 0x66828 bytes each, PRIVATE then GAME), and
  returns true when the session is active (`active` at +0x40 is not INACTIVE) and its host
  address is this machine (`host.info.netAdr.type` at +0x90 is LOOPBACK, 2), the function
  at 0x141EF3F30.
- The client side keeps its own sessions in another array, 0x1555F6520, 0x2A58 bytes
  each (a `LobbySession` of 0x23D0 and more); a second array of the same size starts at
  0x1555F8D88.
- The engine asks `LobbyHost_IsHost` in 82 places, inside `SV_SpawnServer` and `SV_Map_f`
  among them.
- Measured on one PC (2026-11-13): in the menus both host sessions read 0; while a level
  this game started loads, PRIVATE and GAME read 1.
- Not settled: whether a client that connects straight to another host
  (`CL_ConnectFromLobby`, what Xerces' direct connect calls) while it hosts its own game
  lobby still reads 1. `CL_ConnectFromLobby` shuts the menus' level down and preloads the
  host's map, but nothing in it visibly leaves the client's own lobbies. Xerces does not
  rely on it (below).

## Loading a level: when zones link

- `SV_SpawnServer` (0x1421F5D90..0x1421F659E) calls `Com_LoadLevelFastFiles`, which
  first tries `DB_ActivatePreloadedMap` and otherwise calls `DB_LoadXAssets` with its sync
  argument 0: the zones load in the background.
- Inside `SV_SpawnServer`, `sv.state` (0x15767AC90, `serverState_t`) goes to LOADING (1)
  just after the load is started and to GAME (2) near the end; the `svs` field at +0xC00
  (most likely `initialized`) is set before the load, and +0xBFC (most likely
  `baselineCreated`) is cleared during it and set at the end.
- Measured with a read-only probe on a custom map started from the console (DS4C +
  SANCTUM OBITUS, 2026-11-14): when the map's scripts were weighed against the mod's,
  `sv.state`, `svs.initialized` and `sv_running` were all 0. The menus' server had been
  shut down (all three went to 0 within half a second) and the new one came up about two
  seconds after the scripts had been decided. The map is preloaded before the server is
  spawned, so no server state tells a level this game hosts from a host's level it joined.
- Xerces therefore drops a joined host's map-first rule when this game launches a match
  of its own, not by asking at link time (below).

## Launching a match from the lobby menu

- The lobby menu's Solo Game, once a map is picked, runs the engine command
  `lobbyLaunchGame`, about three seconds before the level starts loading (seen through a
  wrapper on the command, 2026-11-14). Xerces' own launch (`party.cpp`) sends the same
  command.
- A client of another host never runs it: the host launches.
- Xerces points the command's entry in the engine's command list at its own function,
  which drops the joined host's rule and calls the engine's (`mod_stack.cpp`), as
  `loader.cpp` does for `quit`. That changes a pointer, no code.

## What the copy protection does not allow

- A detour of `SV_SpawnServer` on the client crashed the game at its first call (the
  menus' level): a null read four calls deeper, with the original's first bytes intact in
  the dump and the four arguments passed as the engine passes them. Taken as the
  protection reacting to changed code in a function it checks. Upstream's detour of the
  same function exists only in the dedicated server build, which has no such protection.
- Changes that worked: pointers in data (the command list), and detours of functions the
  client already hooks or that the signature system resolves (DESIGN.md 4.3).
