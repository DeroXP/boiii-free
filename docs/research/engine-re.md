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
- A running game is measured from outside it: `tools/dev/re/wakeups.py <process>` gives
  each thread's wakes a second and CPU share and the system timer's resolution (nothing
  stopped); `tools/dev/re/sample_thread.py <thread id>` samples where one thread is, the
  way a profiler does (the thread stopped for microseconds per sample, from another
  process, so it can hold none of the game's locks), and `sample_report.py` groups its
  samples by function; `input_latency.py` times a mouse move to the game's own mouse
  counters. `xerces_crash_test threads` writes every thread's ID, name, start and CPU
  time (`<home>\threads.txt`).

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

## Console commands

- `xerces_crash_test cmdlist` (with `-xerces-crash-test`) writes every command in the
  engine's list with its handler to `<home>\commands.txt`; the list from the menus, mods
  off, is in `engine-commands.txt` next to this file: 699 commands, 653 handled by the
  game and 46 added or wrapped by the client (`quit` and `lobbyLaunchGame` among them).
- 38 server commands share one handler, 0x1420DF880, the dispatcher into the server's
  own command list: `map`, `devmap`, `map_restart`, `fast_restart`, `disconnect`, `kick`,
  `status`, `vid_restart` and the rest. 34 commands a player sends to the server share
  0x1420E0FA0, which forwards them: `god`, `noclip`, `give`, `say`, `callvote` and the
  rest. Pointing one entry elsewhere changes that command alone; the work stays behind
  these dispatchers.

## Script built-ins

- The built-ins are in tables of 32-byte entries in `.data`: the name's hash (the same
  FNV-1a the client's `builtin::fnv1a` makes, as in `data/lookup_tables/hash_names.txt`),
  the least and most arguments, a zero, the handler's address (as it is in memory, so a
  dump's base has to be taken off) and a last field that is almost always 0. There are
  several tables; three found from known names:
  - 0x1432C1C90, 73 entries, player methods: `CameraActivate` (exactly 1 argument,
    handler 0x1416C5E50), `CameraSetLookAt` (0 or 1, 0x1416C94C0), `SetOrigin`,
    `SetPlayerAngles`, `SetVelocity`;
  - 0x1432C3970, 171 entries, player methods: `SetClientThirdPerson` (1 to 3 arguments,
    0x1418089B0), `SetLowReady`;
  - 0x143263A50, 340 entries with handlers low in `.text` (0x140xxxxxx), most likely the
    client scripts' (CSC) built-ins: another `CameraSetLookAt` (exactly 1 argument,
    0x140E50ED0).
  A scan stops at the first entry that does not look like one, so a table may go on past
  where these counts end.
- The handlers are protected: control flow flattened into a state machine of 32-bit
  constants, with checks on the process block (`gs:0x30`). What `CameraActivate` and
  `SetClientThirdPerson` change was not worked out from the code. CHARACTERS' preview
  sets both again every half second instead (DESIGN.md 7.13), since either was seen not
  to take once.

## Zombies: a dying actor

- Measured with `xerces_aim_actors` (2026-11-13): a live zombie is an actor (`eType` 0x12)
  with state flags (`nextState.lerp.eFlags`) 0x01 and no ragdoll; the frame it is killed
  the flags read 0x10; a quarter of a second later it is a ragdoll (`pose.isRagdoll`,
  `ragdollHandle`); about a second later its body is a corpse entity (`eType` 0x14) in a
  new slot. Its bounds are 100 units high, from 10 under the feet; the player's eye is 57
  to 61 over the feet of a zombie on the same floor. `bAimTargetVisible` and
  `bRenderedLastFrame` stayed 0 for zombies in plain sight.

## Threads

- The game keeps one thread ID for each of 23 thread contexts in a table of
  8-byte entries at 0x15753F660, and their names in a table of pointers at
  0x1433B93C0: `0_Main`, `2_Backend`, `1_Worker0` to `1_Worker7`, `3_Server`,
  `Database`, `Stream`, `StreamAsync`, `Sound Mix`, `Video Decode`, `Sound Decode`,
  `WebM Decode`, `Netchan`, `Devhost`, `Save Profile`, `Window`, `Input` (the digit
  in front of four of them is the game's). `Sys_IsMainThread` (0x14212AFF0)
  compares the calling thread's ID (cached in its TLS block at +0x88) with entry
  0, `Sys_IsServerThread` (0x14212B110) with entry 10. 0x14212AD30 returns the
  calling thread's context (23 when it has none) and 0x14212ACA0 its name.
- Next to it, per context: the thread's function at 0x15753F720 (cleared when it
  returns), a value at 0x15753F7E0 handed to the naming call, a TLS record of 0x28
  bytes at 0x15753F2C0; events and handles follow from about 0x15753F8A0
  (0x14212B180 waits on the one at 0x15753F8D8 for a millisecond).
- Every context thread starts in 0x14212BB10: it names itself from the names table
  (0x14212B300), points its TLS at its record, runs a per-thread setup
  (0x14227AEB0) and calls the context's function.
- The job system (`[0x1435F5790]`, on the heap) has worker records of 0x68 bytes;
  a worker starts in 0x14000B490, writes its thread ID into its record (+0x08,
  which is +0x70 + i * 0x68 from the job system) and names itself
  `JQ Worker %d (Processor %d)` (0x14000AEF0). 0x14212AF30 copies five of the
  workers' IDs into the table's worker slots right after starting them, before
  the workers have written them: the slots stay 0.
- The names reach no one without a debugger: both naming calls raise exception
  0x406D1388 (`THREADNAME_INFO`, with 6 as the argument count), which the game's
  own handler takes. The client passes each name on to Windows from a vectored
  handler (`exception.cpp`, `thread_name_handler`).
- Measured in the menus (2026-11-14, `xerces_crash_test threads`, which writes the
  table, the worker records and every thread with where it started): 12 contexts
  in use (Main, Backend, Server, Database, Stream, StreamAsync, Sound Mix, Video
  Decode, Sound Decode, Netchan, Window, Input; the worker slots, WebM Decode,
  Devhost and Save Profile at 0), each one but Main a thread that started at
  0x14212BB10; two job workers in the records, three threads that started at
  0x14000B490. The third is in no record and never names itself; at both looks
  of a stalled game it was running in the protection's code (0x15xxxxxxx). What
  it is was not worked out.
- Two more of the game's threads start at 0x142BD5B30 (the C runtime's thread
  start): one waits in the device enumeration (`DEVOBJ.dll`), one at
  0x142BBF8B0. Neither is named.
- How many job workers: the code that starts the job system (0x1422BC380, called
  only from the protection's section; its first bytes jump there) picks a
  processor mask from a count of 1 to 5 (4, 0xC, 0x1C, 0x3C, 0x7C: processors 2
  and up) and stores it at 0x14325238C. The count itself is worked out in the
  protected part. On this PC (24 logical processors, process affinity all 24) the
  game started three threads at the worker entry, and the mask read 0xD in a map
  (not one of the five: something else writes it too). So the game uses at most
  five job workers, here three, whatever the core count.
- What each thread costs (2026-11-14, Shadows of Evil, mods off, standing at the
  start; `xerces_crash_test threads` twice 20 s apart, CPU time per thread,
  `bench\20261113-left\thread-cpu.ps1`), as a share of one core: Main 81%, JQ
  Worker 1 60%, JQ Worker 2 60%, Window 40%, 2_Backend 34%, 3_Server 6%, Sound Mix
  5%, the third worker-entry thread 4.5%, Netchan and Sound Decode 1% each,
  Database 0.2% (it had taken 9 s of CPU while the map loaded). NVIDIA's driver:
  one thread at 90% and 25 more at 8 to 13% each, 3.5 cores in all. The whole
  game: 6.5 cores. The main thread and the two job workers are the ones near a
  core's limit; more job workers is not a setting the game has. A second run, by
  cycles (`QueryThreadCycleTime`) next to the clock-tick times, agreed within a few
  points: Main 72%, the workers 54% each, Window 46%, 2_Backend 30%, 3_Server 5%.
- The Window thread (0x1422BC210) is a message loop: `PeekMessageA` with
  `PM_REMOVE | PM_NOYIELD` until the queue is empty (`WM_QUIT` ends it),
  `TranslateMessage`, `DispatchMessageA`, then `Sleep(1)` and 0x142287510, which calls
  `ShowCursor` until the cursor's count is where the game wants it, every time round.
  With the timer at 0.5 ms (something on this PC had set it) it woke about 840 times a
  second (`wakeups.py`) for about half a millisecond each. Sampled (`sample_thread.py`,
  3000 samples): 72% asleep in `Sleep`, and most of the rest in `PeekMessage`, in
  `ShowCursor` through the Steam overlay's hook of it (`gameoverlayrenderer64.dll`),
  `GetProp`, and DXGI's device-state and redirection checks. The window procedure
  (0x1422BB620) also calls `SetThreadExecutionState(ES_CONTINUOUS |
  ES_DISPLAY_REQUIRED)` on every message it gets (the display is kept on, with
  `r_fullscreen` 1), and on `WM_WTSSESSION_CHANGE` (0x2B1) a lock of Windows adds one
  to `cl_paused` and an unlock takes it off.
- Where the main thread and a job worker were, standing at the start of Shadows of
  Evil (3,000 samples each, `sample_report.py`, functions by `.pdata` and named by the
  nearest known symbol, which is only a hint): the main thread waited 21% of the time
  in `WaitForSingleObject` (for the render backend and the workers), 5.5% in `Sleep`,
  3.6% in `SetEvent`; the most of its own code was 8% in 0x141D07230 and 2.9% in
  0x141D05C50 (both just after `R_StreamPushDisable`, the image streamer's part of the
  code) and about 4% in three functions near `CM_SightTraceThroughTree` (collision
  traces). JQ Worker 1 waited 39% of the time; its most was 6.7% in 0x142911BAC and
  2.8% in 0x142914E50. Nothing stood out as one function to fix. 0x141D07230, the
  main thread's most: under the critical section at 0x157DCD268 (entered at the
  outermost call only, by a nesting count in the TLS block at +0x1D4) it walks the
  set bits of two bit arrays 0x9D80 bytes apart (ending at 0x150A1F280) and looks at
  records of 0x78 bytes at 0x146201900 for each: the image streamer's slots, every
  frame. A place to
  look if the streamer's cost is ever worth a change.
- What the main thread waits for: sampled again with stack scans (2,500 samples), 98%
  of its `WaitForSingleObject` time was `Sys_WaitForSingleObject` (0x14212BC50) called
  from 0x141CD0B50, a renderer function (near `R_AddCmdDrawUIQuads`): the main thread
  waits for the render backend, which waits for the Window thread's Present (below).
  So when the processor holds the frame rate, Present's wait is on the main thread's
  path too.
- The game presents on the Window thread. Sampled from outside
  (`sample_thread.py --scan`, `sample_report.py --chain`), the render backend thread
  (2_Backend) spent 66% of its samples in a level and 88% in the menus inside
  `SendMessageA`: at 0x141C47900 it sends the game's window (`[0x157DF74A0]`) message
  0x8043 with `r_vsync` as wParam (Present's sync interval), and waits. Just before
  it, when `r_vsync` is on, a value at 0x14F464C90 is set and the one at 0x14F437800
  is 2 (the swap chain's setup writes 2 there at 0x141CAD0C1; it read 1 in the dump,
  most likely the window mode), it calls `Sleep` (through the thunk at 0x14212B350) for `1000 /
  r_refreshRate - 2` whole milliseconds (2 at 240 Hz, 14 at 60 Hz; the constant is
  -1000.0 at 0x142F1A5C8): it sleeps most of a refresh before it presents. The dvars
  were named from the dump: each `dvar_t` starts with its name's hash (`CanonHash`,
  FNV-1a from 0x4B9ACE2F with the closing zero), matched against
  `data/lookup_tables/dvar_list.txt`. The window procedure handles three
  private messages with the swap chain kept at `[0x14F4378D8]`: 0x8042 calls
  `IDXGIFactory::CreateSwapChain` (wParam the factory, the device from
  `[0x14F437780]`, lParam the description), 0x8043 `IDXGISwapChain::Present(wParam,
  0)` and 0x8044 `SetFullscreenState(wParam, lParam)`. So every frame's Present waits
  until the Window thread's `Sleep(1)` ends and its `PeekMessage` takes the sent
  message: up to a millisecond and a bit.
- `-xerces-present-wake` (`src/client/xerces/present_wake.cpp`, off unless given)
  points the game's import of `Sleep` at a stub that, on the Window thread and for a
  wait of 1 ms only, waits at most the same millisecond but wakes as soon as a
  message is sent to the thread (`MsgWaitForMultipleObjectsEx`, `QS_SENDMESSAGE`).
  Measured in Shadows of Evil (2026-11-14, mods off, the user's own display settings:
  a 3840x2160 borderless window, 50% render resolution, MaxFPS 240, vsync off; two
  pairs of 30 s runs with the frame probe, `bench\20261113-left\present-ab.ps1`):
  about 150 Presents a second were taken early; the mean
  frame rate stayed the same (204.6 and 206.8 FPS without, 207.9 and 205.3 with),
  the time inside Present grew from 2.0 to 3.0 ms (Present now waits on the GPU's
  queue instead of the loop's sleep: the GPU's queue held the rate), and the frame
  times' standard deviation fell from 0.96 and 0.87 ms to 0.61 and 0.58 ms. p99 and
  the 1% low did not change beyond the runs' own spread. A wait a sent Present
  ended had lasted about 0.6 ms, where a whole `Sleep(1)` lasted about 1.2 ms (the
  0.5 ms timer this PC had): those Presents start about 0.6 ms sooner (the stats of
  a later run through two loads, a `fast_restart` and the menus, which ended
  cleanly; a second reader's review asked for that run). Not tried where the
  processor holds the frame rate (a run with a lower render resolution did not start:
  the test launcher passes no `+set`), nor in the user's own play with mods.
- The Input thread (0x1422BB4A0) makes a message-only window, class
  `CoDBlackOpsInputs`, and loops: `GetRawInputBuffer` into a 0x400-byte buffer at
  0x157DF7520 until it is empty, adding each mouse record's `lLastX` and `lLastY`
  into two counters (0x157DF7498, 0x157DF749C) with locked adds, then `Sleep(1)`. The
  game takes the sums once a frame, so mouse movement can sit up to about a
  millisecond in the raw input queue before it counts.
- `-xerces-input-wake` (same file as `-xerces-present-wake`, off unless given) ends the
  Input thread's `Sleep(1)` as soon as raw input comes (`QS_RAWINPUT`; `GetQueueStatus`
  first, since `GetRawInputBuffer` is no look at the queue). Measured from outside
  (`tools/dev/re/input_latency.py`: a one-count mouse move with `SendInput`, then the
  game's X counter read until it moves; Shadows of Evil, 400 moves each): without the
  flag 1,110 us on average (median 1,440, p90 1,497, most 2,072); with it 258 us
  (median 226, p90 373, most 1,491). The flag's stats: 399 waits ended by input for
  the 400 moves, about 800 waits a second as before (no spin). The price: with about
  3,500 to 4,000 moves a second (`--flood`) the Input thread used 6.4% of a core
  against 1.0% without the flag. Its own stats had about one wait ended per move
  (16,059 for about 16,800 moves in 4.75 s); `wakeups.py` counted 14,118 context
  switches a second (661 without), so each pass switches three or four times, most
  likely on Windows' input lock in its input calls.

## A frame, end to end

What the sections above add up to, for a level (2026-11-14):

1. The Input thread drains the raw mouse input every millisecond into two counters.
2. The main thread runs the frame (the client, the server's work is on 3_Server, the
   scripts), hands work to the job workers (two or three, at most five), and waits in
   `Sys_WaitForSingleObject` for the render backend to be done with the frame before
   (about a fifth of its time at the start of Shadows of Evil).
3. The render backend (2_Backend) turns the frame into draw calls, sleeps before
   Present when `r_vsync` is on, and sends the Window thread message 0x8043.
4. The Window thread takes it at its next look at the queue (after up to a whole
   `Sleep(1)`, or at once with `-xerces-present-wake`) and calls
   `IDXGISwapChain::Present`, which blocks while the GPU's queue is full.

So in a level the GPU holds, Present's blocking is the wait everything else lines up
behind; where the processor holds it, the main thread is the limit and the Window
thread's millisecond is on its path.

## Network timeouts (for the two-PC test)

The values in the dump of 2026-11-04 (the menus), read by matching each `dvar_t`'s name
hash against `data/lookup_tables/dvar_list.txt`. What a join that stalls runs into:

| Dvar | Value | What it bounds |
| --- | --- | --- |
| `cl_connectTimeout` | 120 s | a client's whole connect |
| `cl_connectionAttempts` | 40 | the connect packets a client sends |
| `cl_timeout` | 20 s | a connected client hearing nothing from the server |
| `sv_connectTimeout` | 90 s | the server waiting for a connecting client |
| `sv_timeout` | 30 s | the server hearing nothing from a client |
| `sv_reconnectlimit` | 3 | reconnects |
| `lobby_clientTimeout` | 30,000 ms | a lobby client hearing nothing from its host (a warning at `lobby_clientWarnTimeout`, 5,500 ms) |
| `lobby_hostTimeout` | 45,000 ms | a lobby host hearing nothing from a client |
| `lobby_ptptimeout` | 60,000 ms | the lobby's peer-to-peer link |
| `lobby_clientContentTimeout`, `lobby_hostContentTimeout` | 20,000 ms | content checks in the lobby |
| `reliableTimeoutTime` | 20,000 ms | a reliable message's answer |
| `lobbyMigrate_Enabled` | 0 | host migration (off) |
| `cl_maxpackets`, `sv_maxRate` | 100, 20,000 | client packets a second, server bytes a second |

So a join that fails at about 20, 30 or 45 seconds points at the matching timeout above.
