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
  time (`<home>\threads.txt`). `sample_thread.py --seconds` bounds a run by time (a
  map's load). The client's `-xerces-io-trace` (test only) records every file the game
  opens and every read through its own import pointers, `xerces_io_trace_dump` writes
  them, and `tools/dev/re/io_report.py` sums them up by thread, by second (the reads
  under way at once, how long each took) and by file.

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
  `loader.cpp` does for `quit`. That changes a pointer, no code. After a review
  (2026-11-14): the rule goes only when this game hosts its game lobby at that moment
  (what the lobby menu checks before it launches, `IsLobbyHost(GAME)`), so a client in
  another host's lobby that runs the command anyway (a mod's menu, the console) keeps
  its host's rule; the entry is looked at every second and pointed back when it holds
  the engine's function again (a rebuilt command list), never when it holds another
  wrapper's; and a `map` from the console in a match drops the rule too (`party.cpp`).

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
  of a stalled game it was running in the protection's code (0x15xxxxxxx). Only
  one place starts threads there (0x14000AC1B in 0x14000AB50: for each of the
  count at 0x1435F578C, `CreateThread` suspended with the worker's record,
  `SetThreadPriority` normal, `ResumeThread`), so it is a job worker too, most
  likely worker 0 (its record would be the one before the two read, at the job
  system + 0x08). Sampled with stack scans in a level (2,000 samples,
  `bench\20261113-left\sample-worker0.ps1`): 88% asleep in a `Sleep` loop at
  0x14213C9B0, the rest a scan from the game's code at 0x142194300 and
  0x142197070: `EnumChildWindows` (`NtUserBuildHwndList`, 4.5%), `GetClassNameA`
  and `NtQuerySystemInformation` (which the client's `arxan.cpp` answers): it
  looks over the PC's windows and processes, the way an anti-cheat scan does. So
  this worker never goes back to the job queue, and the game's frame work runs
  on the other two. Why it does not name itself was not worked out.
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
  frame. Read in full (it has one caller, just after `R_StreamPushDisable`): 0x8C00
  slots (35,840) in 1,120 mask words; for each slot set in the first array (in use)
  it takes the slot's image (`+0x70`); when the slot is also set in the second array
  (used this frame) and the image is loaded (its flags' low two bits 3, flag 0x4 clear),
  it clears the image's flag 0x10 and sets the slot in a third array (0x8C00 bytes
  below the second); otherwise, when the slot's last-used frame (`0x150A8A580 + slot
  * 4`) is more than 15 frames old, it sets flag 0x10: the streamer's mark for an image
  nobody has drawn for 16 frames. The cost is the walk over every loaded image's
  record and image, one cache line each, not the masks. A place to
  look if the streamer's cost is ever worth a change.
- Under load (2026-11-14, `bench\20261113-left\load-sample.ps1`: Shadows of Evil
  started at round 40 through `xerces_zm_startRound`, mods off, 166 zombies left in
  the round with the player down among them, about 214 FPS on the HUD): the server
  thread (the zombies' AI and the scripts) used 7% of a core and waited 82% of its
  samples; the main thread 76%, each job worker 56%, the Window thread 48%, the
  backend 29%, the same split as at the start of the level, and the same functions
  on top (the streamer's slot walk 6.8% of the main thread). Nothing grew with the
  zombies on this PC.
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
  most likely the window mode), it calls `Sleep` (through the thunk at 0x14212B350)
  for `1000 / r_refreshRate - 2` whole milliseconds (2 at 240 Hz, 14 at 60 Hz; the constant is
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
  message is sent to the thread (`MsgWaitForMultipleObjectsEx`, `QS_SENDMESSAGE`),
  and only for the loop's own call (return address 0x1422BC302; the Input thread's is
  0x1422BB5D6): a `Sleep(1)` of the thread anywhere else stays a sleep, so a loop that
  does not look at its queue cannot spin (a review, 2026-11-14).
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
| `sv_reconnectlimit` | 3 s | the server time since a client's last connect before the server takes its join (`cfl`); every Zombies level's start waits it out (A map's load) |
| `lobby_clientTimeout` | 30,000 ms | a lobby client hearing nothing from its host (a warning at `lobby_clientWarnTimeout`, 5,500 ms) |
| `lobby_hostTimeout` | 45,000 ms | a lobby host hearing nothing from a client |
| `lobby_ptptimeout` | 60,000 ms | the lobby's peer-to-peer link |
| `lobby_clientContentTimeout`, `lobby_hostContentTimeout` | 20,000 ms | content checks in the lobby |
| `reliableTimeoutTime` | 20,000 ms | a reliable message's answer |
| `lobbyMigrate_Enabled` | 0 | host migration (off) |
| `cl_maxpackets`, `sv_maxRate` | 100, 20,000 | client packets a second, server bytes a second |

So a join that fails at about 20, 30 or 45 seconds points at the matching timeout above.

## A map's load (Shadows of Evil)

Measured on 2026-11-14 from the menus to Shadows of Evil, mods off, the files in Windows'
cache (each load ran after others), 3840x2160, `MaxFPS` 240. Tools: `sample_thread.py
--seconds` on several threads at once from the moment of `map`
(`bench\20261113-left\zoneload-sample.ps1`), the read trace (`-xerces-io-trace`,
`src/client/xerces/io_trace.cpp`, read with `tools/dev/re/io_report.py`;
`bench\20261113-left\io-trace.ps1`), and a frame-time run across the load
(`io-bench.ps1`).

- About 15 s from `map` to the client's connection being active (the journal's
  `in_game`), in four parts:
  1. About 5 s until the client starts loading the level (`loading_map`). The game reads
     about 480 MB of the map's images early in it, then nearly nothing for 3 s while the
     main thread tears the menus down (about 80% busy for a second).
  2. About 4 s of zone loading. The zone files (`.ff`) are read by the Stream thread in
     synchronous 512 KB reads; the Database thread decodes them and spends much of this
     part asleep in short waits: for the sound banks (`SND_LoadSoundsWait` at
     0x142513020; the banks, `zone\snd\all\*.sabl`, are about 500 MB of what the load
     reads) and inside the zone reader. The job workers are busy at the same time.
     Sampled again with `-xerces-fast-loads` (2026-11-14 late morning, `wait-sample.ps1 -Tag
     zones -Threads Database,Main`, from `CONFIRMLOADING` to the server up, 3.3 to 7.7 s):
     the Database thread sleeps 59% of it; of that, 38% in its own idle loop (0x141424F00,
     `Sleep(1)`, nothing queued), 26% in the zone reader's wait for the Stream thread
     (0x1413F0B70), 27% polling for the sound banks (`SND_LoadSoundsWait`, `Sleep(30)` a
     poll) and 7% in another wait on the sound path (0x1425D4B50). So this part is paced by
     the reads and the sound banks, not by the decoding; a shorter poll would only trim each
     wait's overshoot (at most 30 ms). Nor by Main: 70% waiting in the same window, 86% of
     that in the frame limiter between loading-screen frames.
  3. About 4 s of streaming the level's images while the loading screen shows (below); on
     Shadows of Evil it runs inside the client's `CONFIRMLOADING` wait, which, not the
     streaming, ends it.
  4. About 2.5 s to the client's game start (the weapons are registered about a second
     before the connection is active).
- Every read comes from Windows' cache (the files are opened with
  `FILE_FLAG_OVERLAPPED` and without `FILE_FLAG_NO_BUFFERING`) and takes 0.05 to 0.35 ms.
  Never more than one read is under way at a time: the streamer's reads (`ReadFileEx`,
  32 KB to 1 MB, on StreamAsync) go one after another. About 2.2 GB are read in all.
- Part 3 is paced by the loading screen, not by the disk or the processor. The streamer
  asks for a batch of about 8 reads (about 3.5 MB) once every two presented frames (107
  of 119 gaps between batches), 6 to 9 ms after a Present. The loading screen presents
  about 57 times a second while the main loop runs about 225 frames a second and waits
  in its frame limiter 75 to 85% of the time; the render backend and the Window thread
  are mostly idle. So about 470 MB come in at about 115 MB a second, where the same
  streamer read 455 MB in one second just before (part 2's end) and reads 200 MB or more
  a second in the level.
- `com_maxfps` 60, 240 and 1000 change nothing (loading 10.4, 9.8 and 10.4 s): the
  loading screen's pace is its own, about one frame per 16 ms (57 a second is every fourth
  frame of the main loop's 4.4 ms ones).
- Where that pace is decided: `SCR_UpdateScreen` (0x1413E1570), which the main frame
  calls once each time round (0x1420ECE6D). While local client 0's connection state
  (0x145359BC8, `clientUIActives[0]`) is below 11 (active) it draws only if
  `Sys_Milliseconds()` has moved on 16 ms (`mov ebx, 0x10` at 0x1413E15A4) since its
  last draw (0x145778C7C), 33 ms (`mov ecx, 0x21` at 0x1413E15A9) in states 9 and 10
  (`PRIMED`, `LOADING`);
  otherwise it calls 0x14212B350 with 0 and returns. A first scan for compares near the
  clock missed it: the 16 is loaded into a register first. `-xerces-fast-loads`
  (opt-in, `src/client/xerces/fast_loads.cpp`) sets both waits to 0 after checking
  the two instructions.
  `CL_CheckKeepDrawingConnectScreen` (0x1413CCB00) is flattened by the protection.
  (Stack scans of the main thread in that part also pass through 0x14133D3F0: that is
  the console's drawing, which reads the console's text ring of 0x8000 bytes at
  0x1452FDF88 in 0x20-byte steps, called from 0x141339890, which then draws the input
  line. A stale value on the stack, not the streamer's driver: the scans' chains are
  rough, as `sample_thread.py` says.)
- With the screen update's waits at 0 the loading screen presents about 220 times a
  second and the same streaming is done about 3 s sooner (588 MB and 356 MB in two
  seconds), yet Shadows of Evil's level starts no sooner with that alone (the flag's 4x has
  since taken 3 to 4 s off: below) (`bench\20261113-left\io-zod-fast`,
  `fastload-ab.ps1`: 10.2 against 10.1 s of loading). Custom maps, whose images stream
  for longer behind the loading screen, did load sooner (`fastload-maps.ps1` and
  `fastload-maps-rev.ps1`, no mods, both orders): Town Reimagined 15.9 and 16.0 s against
  22.5 and 22.3 s, MW2 Rust 16.9 and 14.8 s against 27.1 and 27.0 s, Backrooms 18.1
  against 21.4 s. With a three-mod stack, whose assets add to what streams, and the solo
  launch without the lobby's waits (`fastload-mods.ps1`): Town Reimagined 11.0 s against
  33.7 s, MW2 Rust 9.9 s against 39.2 s. Shadows of Evil's end is held elsewhere:
- The client's connection state and the server's, read every few ms from outside
  (`tools/dev/re/watch_states.py`, `states-load.ps1`), Shadows of Evil, `map` at 1.35 s:
  the client goes `DISCONNECTED` at 4.79 s (the menus' server down), `CONFIRMLOADING` at
  5.29 s, and stays there; the new server is up (`sv.state` 2) at 9.74 s and the client's
  zones are linked at 9.78 s, but the client leaves `CONFIRMLOADING` only at 14.51 s, then
  `SENDINGDATA`, `CONNECTED`, `PRIMED`, `LOADING` and `ACTIVE` within 0.6 s. So about
  4.8 s of a Shadows of Evil load is the client waiting in `CONFIRMLOADING` after the
  zones are linked (the level's images still stream during it: below). Not the cause (each tried, no change): the frame cap, the
  loading screen's style (`boiii_loadingScreen` 0, 1, 2), when the client stops the
  Zombies intro (`client_patches.cpp` waits 15 s after the preload; a test build with 0
  and 5 s), `waitForStreamer`.
  `CL_CheckForResend` (0x14134B990), which handles the early connect states, starts in
  the protection's code. `reliableResendTime` is 300 ms and `join_retryDelay` 1,000 ms.
  The console, read from its window every 100 ms (`bench\20261113-left\console-watch.ps1`),
  prints the server's "Game Initialization" and its scripts at about 9.3 s and then
  nothing at all until "Connected" at 14.9 s: a quiet 5 s, about the length of
  `lobbyTimerZMStartInterval` (5,000 ms). Set to 0 from the console before `map`, or 4 s
  into the load, it changed nothing (whether the second one reached the game was not
  seen). `lobbymsg_prints 1` printed nothing. (What it is: two and three items down, `sv_reconnectlimit`.) The only constant writes of
  the connection state in the readable code are `LOADING` (0x141326EA3) and
  `DISCONNECTED` (0x14135945D), so the others are made in the protection's code or
  through a pointer.
- Not a fixed time either: on Origins (`zm_tomb`, `states-tomb`) the client entered
  `CONFIRMLOADING` at 5.28 s, the server came up at 8.50 s and the client left at 11.51 s:
  3.0 s after the server (4.8 s on Shadows of Evil), 6.2 s after entering (9.2 s). Nor the
  sound banks: Shadows of Evil's (`zm_zod.all.sabl`, 211 MB) is read by 39.4 s of its trace
  and the console says its sounds are loaded right after, about 5 s before the connect.
- What it is (2026-11-14 morning): a wait on the new server's clock. `watch_states.py` now
  prints the server's `level.time` (0x14A1B6C14, what GSC `gettime()` reads) with each
  change: it restarts at 0 when the server comes up, runs 1:1 with real time, and the
  client is let go at `level.time` 4,950 on Shadows of Evil, 4,800 on Town Reimagined and
  MW2 Rust, 3,150 on Origins, every run. `timescale` set once the server is up (with
  `devmap`; the client already takes the dvar's cheat flag off): 4 ended the wait at
  11.4 s instead of 14.5, 2 at 12.5 s, 1.01 changed nothing; so it scales with game time,
  roughly (2 and 4 do not fit one constant; an early guess was a real-time beat on the
  client's side: the server's check, three items down, measures its own time). What it is not: the thread CPU watch (`tools/dev/re/thread_cpu.py`,
  `bench\20261113-left\states-cpu.ps1`, every 200 ms) shows Main at 12 to 25% (the
  loading screen) and the server thread near idle through the wait; sampled
  (`wait-sample.ps1`), Main is 81% in the frame limiter's wait and the server thread 98% in
  its wait for the next frame's signal (0x1421F7790, called from the thread's loop at
  0x1421F84C0). Only the protection's worker 0 is busy, 100% of a core: with the read
  trace in the same run (`-ExtraFlags -xerces-io-trace`), it reads the 71.9 MB
  `RemoteDesktopCompanion.exe` at 8.9 s and works on it until about 17.5 s; but the client
  left at 14.4 and 14.5 s in two runs whose worker started 1.4 s apart, so not that. Nor
  the online service: a test build traced every exchange with the client's emulated
  Demonware (send, receive, service and task): the last before the connect is a rich
  presence update (service 68) as the load starts, then only the STUN pings every 15 s.
  Nor `lobby_hostIntervalState` (5,000 ms, the host's lobby state interval; 100 changed
  nothing). Nor the server's flags after `levelTime` in `server_t` (`isUILevel`,
  `isRunnable`, `allowSelfTick`, `allowNetPackets`, 0x15767ACA8, now in `watch_states.py`):
  the new level's server comes up with all but `isUILevel` set and none changes when the
  client is let in (the menus' server has `isUILevel` 1). Nor a script's wait: a test build
  (reverted) wrapped the server VM's `Wait`, `WaitTill` and `WaitTillFrameEnd` and named
  each site with `script::get_script_frames`, from the server's start to level.time 9,000
  (`bench\20261113-left\wait-trace-zod2.txt`). In those seconds the level's scripts set up
  its scenes: `scene::on_load_wait` at 150, `csceneobject::initialize` (41) at 250, and
  their models spawn through `spawner::global_spawn_throttle` (`util::spawn_model`), 4,003
  waits of 0.05 s from 250 on and still going at 9,000; beside them the usual
  `util::wait_network_frame` loops (`spawn_throttle_reset`, `load::count_network_frames`,
  `zm_net::network_choke_thread`), `globallogic_utils::gametimer` every second and
  `globallogic::graceperiod` (15 s). No wait ends just before the client is let in;
  `callback_playerconnect` runs at 4,850, the client's own arrival. So the hold is in the
  engine, on game time, its length the map's (and with the three-mod stack 3,300 on Town
  Reimagined), not a script waiting for something. Every dvar dumped twice in the wait and
  once after (`xerces_crash_test dvars`, `dvars-wait.ps1`): nothing changes in the wait;
  across it only the client's own (`cl_ingame`, `all_players_are_connected`, `cg_fov` and
  the like). Nor a packet: a test build (reverted) logged every connectionless command the
  client's `handle_command` sees, both ways, through a Shadows of Evil load: the only ones
  are two `cfl`, at the very moment the load starts (the same millisecond as the client's
  `loading_map`) and 1.8 s later, with the client in `CONFIRMLOADING` and the new server
  still down (`sv.state` 0); nothing at all when the client is let in, and no `connect`
  (the one `connect` of the run is the menus' own at the start). So the request to join is
  made early, and the client moves on, with no packet seen here, when the server's clock
  reaches the mark (the server checks it: "The hold, found" below; the `cfl` it takes past
  the limit was not seen in this log).
- The hold, found (2026-11-14 late morning): `sv_reconnectlimit`. The string "cfl" is
  loaded at two places in the dump: 0x14134BCAC, in `CL_CheckForResend`, which builds the
  packet ("cfl", the player's XUID as an int64, a short) and sends it with
  `NET_OutOfBandData`; and 0x1421F738C, in the server's connectionless-packet dispatch
  (between "connect", which goes to `SV_DirectConnect`, and the next), which calls
  0x1421ECAC0. That handler returns at once for the menus' server (`sv.isUILevel`), answers
  `EXE_MIGRATION_IN_PROGRESS` during a host migration, reads the XUID and the short, finds
  the player's slot in the game lobby (0x141ECC460) and, with the slot below
  `com_maxclients`, compares `svs.time` (0x15767A384) minus the slot's client record's
  time at +0x55E4 (its last connect; records 0xE5170 bytes apart from the pointer at
  0x15767A398) with `sv_reconnectlimit` (the dvar pointer at 0x157736840, value 3) x 1000:
  less, and the `cfl` is ignored. Past it, a party check (0x141ECE9F0), then it counts the
  records in state 3 or above and, with one or all of them there, calls 0x1421E8CE0 (the
  start). Both dvars named from the 2026-11-04 dump: the pointers lead to dvar records whose
  debug names are `sv_reconnectlimit` and `com_maxclients`. Tried, mods off: with
  `sv_reconnectlimit 0` set before `map`, Shadows of Evil let the client in at level.time
  1,950 instead of 4,950 (3 s sooner, as the limit says), and Origins at 150 instead of
  3,150 (with the flag, whose 4x then ran 39 ms); the rest on Shadows of Evil (about 1.9 s) also runs on the game's clock (at 4x it
  took 0.4 s). Not used: at 0 the host is in at level.time 150 to 1,950, sooner than the
  game ever lets anyone in (the limit holds every client at least 3 s), and the level's
  scripts may count on that; the flag's 4x keeps the arrival at the usual level.time (the
  connect at 4,850 either way, the client seen leaving at 4,950). With the limit at 0 and the
  4x the level started another second sooner in a test build (Shadows of Evil `in_game` 8.9 s
  against 9.9 s in that run; the flag as committed: 8.8 to 9.6 s against 12.9 s, TESTING.md;
  Origins 6.3 s after `map`).
  Watched from outside (`watch_states.py` now prints client record 0's state and its time at
  +0x55E4 with `svs.time`): the new server zeroes the record (state 0, time 0) as it starts,
  and `svs.time` restarts at 0 with level.time, so the limit runs out at `svs.time` 3,000.
  Past it the handler connects the client at once: 0x141ECE9F0 is the connect itself (record
  state 3, `svs.time` at +0x55E8). Origins: state 3 at `svs.time` 3,000 exactly. Shadows of
  Evil: only at 4,850, so there the join arrives or is taken 1.85 s after the limit ran out.
  It arrives then: a test build (reverted) logged every "cfl" the client sends
  (`NET_OutOfBandData`, 0x14211AB40): the client sends it again every 3 s of its own clock
  (the one `timescale` scales; longer in real time while the zones load). Shadows of Evil:
  sent with the server still down, again 2.5 s later, then at level.time 1,900 (refused, under
  3,000) and 4,900 (taken); Origins: at 50 (refused) and 3,150 (taken). So the hold is the
  3 s limit plus however far the client's next resend lands past it, which the load's own
  timing decides: hence the map-to-map spread (3.15 to 4.95 s) and why 4x shortens all of
  it.
  A `map` from inside a level
  goes through without such a wait, and so does the way back to the menus
  (`menu-return.ps1`, `disconnect` from Shadows of Evil: `CONNECTING` 1.2 s after it, the
  menus' server loading 3.4 s later and up at 5.7 s, the client `CONNECTED` at its
  level.time 200 and active 1.2 s after that).
- The wait hid the texture streaming. With the server's clock at 4x only while the client
  waits, and 1 again the frame it moves on, the wait took 1.1 s but `LOADING` then took
  5.9 s ("Awaiting textures" is `ls_status` across it), and the level started 2 s later
  than without. With the every-frame loading screen (`-xerces-fast-loads`, which streams
  about 3 s sooner) the same 4x left `LOADING` at 0.05 s: Shadows of Evil from `map` to
  active in 8.4 s against 11.9 s with the flag's other parts. At 8x the wait was 0.53 s
  but the level started only 0.05 s sooner than at 4x. The flag now does this for a Zombies
  level when no one else is in the lobby (docs/DESIGN.md, "Faster loads").
- The read trace also shows the game reading other programs' executables while it loads: 68.6
  MB of `RemoteDesktopCompanion.exe` (Meta Horizon's remote desktop, running on this PC)
  and also `OVRServer_x64.exe`, `explorer.exe`, `nvcontainer.exe`, `MQRDCrashpadHandler.exe`
  and more, in synchronous reads on the unnamed job worker that started at 0x14000B490: the
  thread found scanning the PC's windows and processes (Threads, worker 0). So that scan
  also reads the executables of the programs it finds, during a level's load too.
- Part 1 holds two of the lobby's launch waits: `lobbyLaunch_waitForClientAckDelay`
  (2,000 ms) and `lobbyLaunch_gameLaunchDelay` (700 ms), there for the other lobby
  members' games to hear of the launch (`...OnConnect` versions: 800 and
  `lobbyLaunch_fadeToBlackDelayOnConnect` 100; `lobbyLaunch_fadeToBlackDelay` 0). From the
  console before `map`: the ack wait at 0 brought `loading_map` from 5.5 s to 4.2 s, both
  at 0 to 3.6 s (13.3 s in all against 15.5); the launch wait alone, and
  `lobbyTimerZMStartInterval` (5,000) or `lobby_clientLoadingIntoUICheck` (1) at 0,
  changed nothing. `-xerces-fast-loads` zeroes the two for a launch with no other player
  in the lobby. What is left of part 1 then (about 2.3 s from the command to `DISCONNECTED`)
  is the engine swapping assets: Main sampled through it (`wait-sample.ps1 -From 0.6 -For
  2.4 -ExtraFlags -xerces-fast-loads`, `bench\20261113-left\wait-sample-teardown`) is 38%
  waiting for the render thread (from `CL_SetupForNewServerMap`), about 18% in the texture
  pool's reference counting (`Gfx_TexturePool_ReleaseRef`/`AddRef` and a routine they call
  at 0x142BC47F0, under `Image_AssignDefaultTexture` < `DB_OverrideAsset` <
  `DB_ActivatePreloadedMap`, and under `DB_UnloadXZone` < `DB_ReleaseXAssets` for the
  menus' zones), 3% freeing memory. Work, not a wait.
- Streaming settings tried (each set from the console just before `map`, one load each,
  `bench\20261113-left\loadtime-dvar.ps1`; the console's answers say their ranges):
  `waitForStreamer` (0 to 2, default 1) at 0, 1 and 2: 15.5, 15.2 and 15.4 s, no change.
  `streamMBPerSecond` (0 to 1000, default 0, which is no limit): 30 made the load 1.8 s
  longer, 1000 changed nothing. `sv_mapSwitchPerloadFrontendStreamerFrames` (default
  600) at 0: no change. `r_streamTextureMemoryMax` is 800 and `r_stream` 7. The level
  starts on the zombies' black screen, so what streams in its first seconds is not seen.

## A start (to the menus)

Measured on 2026-11-14, mods off, the files in Windows' cache (`bench\20261113-left\startup-watch.ps1`,
`startup-io.ps1`, `startup-sample.ps1`): the console window read from outside every 100 ms
(`console-watch.ps1`), the read trace, and the Database thread sampled from the moment it
exists (found by the name the client gives it, `tools/dev/re/find_thread.py`).

- About 7.6 s pass before the client logs its first phase (`symbols`); the menus come at
  about 24 s. In between, the menus' zones load one after another (`core_pre_gfx`,
  `core_patch`, `core_gfx`, `core_post_gfx`, `core_common`, `core_specialists`,
  `core_frontend` and their language zones), each 0.7 to 1.6 s by the console's own lines.
- About 1 GB is read, from the cache, at 100 to 180 MB a second with the disk idle 96% of
  the time and never more than two reads under way: the reads are not what takes long.
- The Database thread is asleep about 70% of that time and blocked on a lock about 14%
  (inside d3d11, creating resources while another thread holds the device), working about
  20%. Its sleeps, by where they return to: its own loop waiting for the next zone
  (`Sleep(1)` at 0x141425017, 40%); `SND_LoadSoundsWait` polling for the sound banks
  every 30 ms (`Sleep(30)` at 0x142513049, 24%); the zone reader's wait for the next part
  of a file (under the stream call at 0x1413F0CE0, 19%); and a sound stream wait polling
  every millisecond (0x1425D4D52, 13%). So the menus' load waits on sound banks, on the
  zone requests coming one at a time, and on the graphics driver's lock, not on the disk
  or on decoding. Shortening the 30 ms poll would save its overshoot only, about 15 ms a
  wait.
- The 7.6 s before the client's first line: the main thread, sampled from the moment the
  process exists (`early-sample.ps1`, a deep stack scan), waits about 4.5 s (from about 2.3
  s to 6.9 s) in an out-of-process COM call, made from a static initializer of the game
  (under the C runtime's start, before `WinMain`). The function at 0x142BC15F8 creates
  the WMI locator (`CoCreateInstance` with `IUnknown`, then `IWbemLocator`) and connects to
  `\\.\root\CIMV2`, `\\.\root\CIMV2\Security\MicrosoftVolumeEncryption` (BitLocker) and
  `\\.\root\wmi` (with SetupAPI near: a description of the PC). For a user who is not an
  administrator the BitLocker namespace answers only `WBEM_E_ACCESS_DENIED` (0x80041003),
  after 5,045 ms (measured from PowerShell; root\CIMV2 takes 61 ms, root\wmi 24 ms).
  `-xerces-fast-loads` answers that one connection with the same code at once while the
  process is not elevated: the client's first line came at 2.6 s instead of 7.6 s, the
  menus at 18.9 and 19.2 s instead of 24.5 and 23.9 s (`start-ab.ps1`).
- With that answered, a start's threads (`startup-cpu.ps1`, every thread's CPU every 500 ms
  from the moment boiii.exe exists, with `-xerces-fast-loads`): the main thread busy from
  0.5 to 8.7 s (the client's and the game's set-up; the Database thread starts at 6 s);
  then about 2.6 s, 9.3 to 11.9 s, with no thread busy at all; then the menus' zones (the
  Database thread and the job workers, 12 to 18.4 s), the render thread at 100% (19.5 to
  21 s, with "Mode Preload") and the menus. The main thread through the quiet stretch
  (`startup-gap.ps1`): 77% asleep, in two loops of the game's start: one in
  `SND_LoadSoundsWait` (its `Sleep(30)` poll, reached from 0x1420EEE5A: the menus' sound
  banks) and one at 0x1420EEE3F, in the function at 0x1420EEB10, which is in the
  protection's encrypted code (it reads as noise in the dump), so what that loop waits for
  is not read. The read trace of an earlier start (`io-startup`, before the BitLocker
  shortcut, so 5 s later) has the same hole: reads end with `core_ui.ff` at about 9.9 s and
  the next is `core_frontend.ff` at 12.66 s, nothing at all in between, and the sound banks
  (opened buffered and overlapped, read in milliseconds) are not slow. So a start stands
  still about 2.7 s between two of the menus' zones, waiting on a timer or a flag. A next
  lead: the image has a `FAKE_INTRO_SECONDS` string and a `com_introPlayed` dvar (a
  minimum time for an intro the launch skips with `-nointro`?). `+set com_introPlayed 1` on
  the command line changed nothing: `symbols` to `frontend` 16.5 s against 16.4 s (unless
  the start reads it before the command line's dvars are set). The stretch lines up with the
  game's first message to the emulated lobby server: in the online-service trace of a
  start (`dw-trace-zod.txt`, the test build from "A map's load") the 28-byte header goes out
  9.3 s after the launch and its 26-byte answer is read 3.0 s later, at the end of the
  stretch. The emulator queues that answer (`lobby_server.cpp`, the header ack) within its
  50 ms tick, and the game's `select` calls are all polls with a 0 ms timeout (a test build,
  reverted, logged 286 of them through a start, none blocking; making a waiting emulated
  socket wait in short steps changed nothing). So the game itself reads the answer 3 s
  later: a timer or state of its online code, not the emulator's speed. The same log has
  no `select` at all from 2.5 s to 16.5 s after the client's first line, the quiet stretch
  included: the online code's pump runs from the main loop, and the main thread is in the
  start's own code then. So the start's sleep loop (0x1420EEE3F) most likely waits for
  something only that pump moves on (the lobby connection's answer would fit) and, since
  it does not pump, ends by its own timeout, about 3 s. Not proved: the loop is in the
  protection's encrypted code; what it polls would have to be read at run time. (Read
  below: it waits for a zone whose sound banks take about 2.7 s; the online pump is likely
  just stopped alongside, not the cause.)
- Read at run time (late morning): the code is encrypted again once it has run (read at the
  menus it is still noise), but read from outside while the start is inside it, 10.5 s
  after launch (`bench\20261113-left\read-code-live.py`: the game's image found by its size
  and `MZ`, then ReadProcessMemory), it disassembles. The loop at 0x1420EEE34 sleeps (a
  call through the protection) until `0x141422620(0x141426E00(4))` is true, then calls
  `SND_InitAfterGlobals` (0x14250BE70). 0x141426E00 asks 0x141426C90 (near
  `DB_ExpandZoneList`) for the name of a zone, 4 being the kind; 0x141422620 walks the
  loaded-zone table (65 entries of 0x60 bytes from 0x149910B80, the state at +0xB8) and is
  true once a zone of that name is in state 2 or 3: a `DB_IsZoneLoaded`. So the start waits
  for a zone to be loaded, and in the read trace nothing is read during the wait:
  `core_frontend.ff` starts at its end. What holds that zone's load (asked late; the online
  pump, stopped meanwhile, would fit) is the next thing to find.
- The Database thread through that pause (`pause-db-sample.ps1`, found by name, the engine
  base by the same scan): in the middle of a zone's load (`DB_LoadXFile`) and asleep 60%
  of the time, in `SND_LoadSoundsWait` (44% of that), another wait on the sound path
  (0x1425D4B50, 17%) and the zone reader's wait (32%). So the zone being loaded waits for
  its sound banks, the start's main thread waits for that zone, and no file is read. A
  lead: the sound banks' load may need something the main loop runs each frame (its sound
  update), which does not run while the start sleeps, so the three wait on each other
  until something gives after about 3 s. Not proved. The Stream thread through the same
  pause (`pause-db-sample.ps1 -Thread Stream`): 99.8% waiting for work in its loop
  (0x142122F40): nobody asks for the sound banks' data in those seconds. "Sound Mix" (98%,
  its mixing loop's wait) and "Sound Decode" (99.7%, waiting for work) are idle as well:
  every thread on the way waits and none works, so the request that would move the banks
  along is not made while the start sleeps. Which code makes it: `SND_LoadSoundsWait`
  (0x142513020) loops, `Sleep(30)` a pass, while the sound system is active and the byte at
  0x1434BF5FC is 0, calling `SND_UpdateWait` each pass; the one write to that byte a scan
  of the readable code finds (direct `mov` to it; not the protected code, not through a
  pointer) is in `SNDL_Update` (0x142509260: the result of `SND_BankUpdate`), and `SNDL_Update` is
  called only from `SND_Frame` (the main loop's sound update) and `SNDL_GameReset`. It
  looked like a wait nothing could answer (the main thread asleep in the start's loop), but
  it is not: a test build (reverted) that logged every `SND_Frame` call saw it run on the
  main thread about 46 times a second right through the pause (1,161 calls in a start, the
  first about 10 s after launch, no gap over 300 ms in the pause): the start's loop keeps
  the sound update going. So `SND_BankUpdate` runs all along and still takes about 2.7 s to
  report the banks done, with no file read and no thread busy meanwhile: what it waits for
  (a time, a state of the sound library) is the next question. `SND_UpdateWait`
  (0x142217490), called each pass of `SND_LoadSoundsWait`, takes a spin lock, notes
  `Sys_Milliseconds` and waits for the sound task at 0x1433BBD20, with no timeout of its
  own. Another test build (reverted) logged each change of `SND_BankUpdate`'s result
  through a start (`bench\20261113-left\bank-log.txt`; the process started at about
  11:51:46.1): no call at all until 10.4 s in (so the pause's first second or so comes
  before the sound update runs), then "not done" for 66 calls (1.4 s, about 48 a second),
  "done" at 11.75 s, where the pause ends; later zones' banks take 0.2 to 0.9 s each the
  same way. So the pause is about 1 s before the sound update starts, then the menus' bank
  advancing one step a sound frame at the start loop's pace. Faster passes of that loop,
  or the sound update starting sooner, are where those seconds would be. The loop's sleep
  is `kernel32!Sleep` itself, called through a pointer the protection keeps in the game's
  data (0x15AA69D74, read from a running game), not through the import table: shortening
  only that call would mean changing that slot or hooking `Sleep` for the whole process,
  both a change to what the protection watches, so it is left.
