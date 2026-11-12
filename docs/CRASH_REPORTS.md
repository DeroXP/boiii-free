# Crash reports

Decided on 2026-11-05 (D21, DESIGN.md 10.5 and 5.10): when the game crashes,
Xerces sends a crash report to the Xerces backend on Railway, where the
developer studies it. Only while the launcher setting "Send crash reports" is
on; it is on by default. The first run's privacy step and Settings > Privacy say
what is sent and have the switch. With it off nothing is sent, and testers
report crashes at <https://github.com/DeroXP/boiii-free/issues> with the
diagnostics bundle (Xerces > Diagnostics > Copy report).

The client and the backend implement this contract exactly. The backend side is
`backend/` on branch `launcher/crash-backend`, not deployed yet: the service
Railway runs (`xerces-design`) has no crash routes, so the client sends no
report until it has (`GET /v1/crashes` below). The client side is
`src/xerces_core/crash_report.*` and `crash_outbox.*`, the game's crash handler
(`src/client/component/exception.cpp`), the launcher (`services.cpp`, Settings,
first run) and `xerces_cli crash list|send`.

## What is sent

- The crash: its kind, code and exit code, the module and offset it happened at,
  the thread (main, server, auxiliary), up to 32 frames as `module+0xoffset`,
  and the launch journal's phase.
- A stacks-only minidump (`MiniDumpNormal | MiniDumpWithThreadInfo`): the
  threads, their registers and stacks, and the loaded modules' paths. No data
  segments and no memory the stacks point to, which can hold tokens and other
  players' addresses. At most 2 MiB; larger ones are left out. The stack memory
  itself is raw: it can hold bits of what the game was doing (a path, a name,
  part of a message). Before the dump is queued the client masks, keeping every
  length so the dump still opens: the name after every `X:\Users\` anywhere in
  it (module paths, PDB paths), and every whole-word copy of the Windows user
  name and of the player names the game knew (three characters or more, UTF-16
  and UTF-8) in the module names, the PDB records and the stacks
  (`xerces_core::mask_minidump`). Nothing else in the stacks is scrubbed. Bytes
  that are not a minidump are left out.
- The launch: game mode, map, plan id, the mods (id, name, version), the flags
  (names only), safe mode.
- Versions: Xerces' version, git commit and whether the tree was dirty; the
  game's exe checksum and Steam build.
- This PC: Windows version, CPU name, logical CPUs, RAM, GPUs with driver
  versions, the primary display's mode.
- Logs: the launch journal, and the last lines of the game's log (200), the
  launcher's log (100) and the console (200), each at most 64 KiB. Chat lines
  never enter the console lines the client keeps (`chat.cpp`), so neither the
  game log nor a report has them.
- The game's own text report (from 0.1.0-beta.22, "The text report" below), at
  most 64 KiB from its start, and three lines taken from it: why the report was
  written (`exception`, `freeze` or `fatal_error`), what happened in words, and
  the script that was running. They are scrubbed as the logs are.

Before anything leaves the PC the client removes from the logs: the profile
folder (written `%USERPROFILE%`), any other `X:\Users\<name>` (written
`C:\Users\<user>`), the Windows user name, player names, Steam ids, values after
`xuid`, server addresses and favourites' names (the diagnostics bundle's
redaction, `xerces_core/log.hpp`, plus these). The player names are the one in
`launcher.json` and the ones the game knew: its own name (from `properties.json`
or Steam, which `launcher.json` often does not hold) and the players it was in a
match with. The game keeps those in memory only and puts them in its crash note;
its console lines already show them as `<player>`. The dump is masked as above.
The server checks again that no `C:\Users\<name>` path survives.

Never sent: usage data or analytics, Steam ids or the game identity (XUID), full
crash dumps, settings, packs or files. The full dumps stay in
`%LOCALAPPDATA%\boiii\minidumps` for local use.

Reports are tied to an install id: 32 random hex digits from the system's random
source in `<home>\config\install_id`, made when the first report is about to go
to a server that takes them, never derived from hardware or accounts. An install
without one never sent a report, so "Delete my crash reports" has nothing to ask
the server then. "Delete my crash reports" deletes every report of that id. The
server keeps reports 90 days and stores no IP address (it uses the address in
memory for rate limits only).

## The text report (2026-11-12, beta.22)

Before beta.22 a crash left a dump and a zip, a freeze a text file with the main
thread's stack and one line for each other thread, and a fatal engine error
nothing but the console's last lines. Reading any of them meant opening a dump.
On 2026-11-12 a server thread stuck in a script had to be dug out of a freeze dump
by hand, and the report's version line was of no use for telling two builds
apart. Now each of the three leaves a text file in
`%LOCALAPPDATA%\boiii\minidumps`, next to its dump, that opens with what
happened and what the game was doing (`exception.cpp`, from `session_text`):

| What | Files | Kind in the report sent |
| --- | --- | --- |
| A crash | `crash-<time>.txt`, `crash-<time>.dmp`, `xerces-crash-<time>.zip` (the dump and the same text as `info.txt`) | `exception`, reason `exception` |
| A freeze (no frame for 30 s) | `freeze-<time>.txt`, `freeze-<time>.dmp` | `unknown_exit`, reason `freeze` |
| A fatal engine error (`Com_Error` fatal, `Sys_Error`) | `fatal-<time>.txt`, `fatal-<time>.dmp` (every thread, new) | `unknown_exit`, reason `fatal_error` |

Every text report has, in this order:

- What happened, in a sentence ("invalid memory access: tried to write to 0x18
  (null pointer) in BlackOps3.exe+0x141D3DA12 on the main thread"; for a freeze,
  where the main and the server thread are; for a fatal error, its words), the
  diagnosis when there is one, and the script that was running.
- The session: Xerces' version and when this client was built (the PE header's
  time, which tells two builds with one version label apart), the time, Windows,
  how long the game had run, the journal's phase, the mode and map, the mods in
  load order with their Workshop titles (the main mod first), the launch flags,
  and the ids of the main and server threads (to find them in the dump).
- The scripts each VM was running, innermost first, as `function (file line
  12)`.
- The stacks. A crash: the crashing thread's (48 frames, with function names and
  source lines where symbols know them), the bytes at the crash address and the
  registers. A fatal error: where it was raised. A freeze: every thread's, see
  below.
- Memory (the PC's RAM and commit left, what the game holds and the most it
  held, the zone pools when the larger zone memory is on) and the settings that
  change what the game does (the upscaler, the Zombies options, the frame cap,
  the display mode).
- The console's last 80 lines. This part stays on the PC: the report that is
  sent has the console's lines already.

A crash's text is put together under a fault guard: if reading the game's state
faults again, the report says so and is still written and sent. The note for
the launcher is written before the text, without it, and again with it. The
text file is written on its own before the zip, which a second fault has cost
before (2026-11-05). The guard is a call: this build's compiler (clang, without
`-fasync-exceptions`) catches a fault inside a function a `__try` block calls,
not one raised by the block's own instructions, so every guarded read is in a
function of its own (`run_guarded`, `xerces_safe` in `safe_memory.hpp`, the
stack walk's read). `xerces_crash_test guard` reads a bad address that way.

The crash handler itself has two minutes. It reads the game's state on a thread
that has just crashed, and the freeze watchdog stands down once a crash is being
handled, so a handler stuck on something the crashed code held would leave a
game that neither plays nor closes. If the handler (or a fatal error's report)
has not reached its dialog or returned after 120 s, the watchdog ends the game
with the crash's code; the dump is the first thing written, so it is there. The
dialog is not timed: it waits for the player.

A freeze's report is written by a thread of its own, so whatever it waits for,
the watchdog still closes the game on time. It reads every thread's stack (each
thread stopped only for its own walk, up to 32 frames, one step through code
without unwind data), and writes the file twice: first with addresses by module
and offset only, from a list of modules the watchdog keeps while the game runs
well, so nothing is asked of Windows that a frozen game may hold (the loader's
lock, the symbol engine); then again with function names, the scripts running
and the session. The note for the launcher is written with the first version,
before the dump, and again with the second. The main and the server thread come
first and in full, then every other thread that is running, then the ones that
wait in a system call, with threads at the same place listed once ("12 threads
at the same place"). If a stack walk does not come back in two seconds, the
watchdog starts the stopped thread again, so the report cannot turn a long stall
into a freeze.

The watchdog's own way to the end of a frozen game takes no lock and no memory
(a second read of this code on 2026-11-12 found that it logged, and asked the
journal for its phase, before it ended the game: either waits forever if the
frozen thread holds the heap's lock or the log's). The report's thread is made
when the game starts and waits for an event; whether the game is loading is
noted while frames still come; the lines for the logs are written by a thread
that is waited for one second. `MiniDumpWriteDump` stops every other thread of
the process while it works, so every dump the game writes leaves the watchdog
out (a thread left out of a dump is left running), and the stack walk never
stops it either. `xerces_crash_test heap` freezes the game with the heap's lock
held: no report can be written, and the game still closes after 60 s.

A stall's report gives its place among a game's three reports back when it is
taken back; three long stalls used to leave a later crash without a report.

To see them without waiting for a real one: start the game with
`-xerces-crash-test` and type `xerces_crash_test` (a crash),
`xerces_crash_test fatal`, `xerces_crash_test freeze` (the game closes after
60 s), `xerces_crash_test stall` (40 s without a frame: a freeze report that
is taken back), `xerces_crash_test heap` or `xerces_crash_test guard` (above).

## How a crash becomes a report

1. The game's crash handler, when the launcher started the game (`XERCES_HOME`
   is set) and the setting is on, writes `<home>\crash\pending\<report_id>.dmp`
   (the stacks-only dump) and `<report_id>.json` next to it (code, module,
   offset, thread, frames, phase, the console's last lines, the flags, the game
   log's file name, the journal, the player names to hide, and from beta.22 the
   reason, the summary, the script and the text report). Upstream's own
   dump and zip stay as they are. A fatal engine error (`Com_Error` with
   `ERR_FATAL`, `Sys_Error`) writes the note alone, kind `unknown_exit`: the
   frames from where the error was raised, and the first frame outside the
   client as module and offset. Its dialog's "Restart game" ends the process
   with exit code 0, which alone would read as a clean exit. The dialog says the
   report is sent when the game closes (the launcher is open) or when Xerces
   opens next; with the setting off it points to GitHub and the bundle. A freeze
   (no frame for 30 s, a watchdog thread in the client) is reported the same
   way, kind `unknown_exit`: a dump of every thread as it stands, the main
   thread's frames, every thread's stack in the text report, and a last console
   line "[Freeze] ..." that says where the main and server threads are. The same
   dump and text report go to the minidumps folder whatever the setting. If frames come back, the pending report is removed; if not, the game
   ends itself with exit code 0xC0000194 (STATUS_POSSIBLE_DEADLOCK) 60 s after
   its last frame, 180 s while the journal says it is loading (2026-11-08).
2. When the game the launcher started exits, the launcher counts it as a crash
   when the client wrote a dump, or the exit code is not 0 and the journal does
   not end in `clean_exit` (`xerces_core::exit_is_crash`). Each pending crash
   becomes a report in `<home>\crash\outbox\<report_id>.json`; a crash without
   one gets a report from the exit code (`fail_fast` for 0xC0000409,
   `out_of_memory` for 0xFFFFFFFF after the out-of-memory box, else
   `unknown_exit`), once per launch.
3. A worker thread sends the outbox, oldest first: never on the UI thread, never
   holding up a launch, each step timing out after 15 s. Before the first report
   of a run it asks the server whether it takes crash reports
   (`GET /v1/crashes`); when it does not yet, nothing is sent, the notice says
   the report waits on this PC, and the launcher asks again in an hour and at
   every start. Otherwise a notice says "Crash report sent" with the server's
   id, or that it is sent later. A failed send is tried again in 10 minutes
   while Xerces is open (or after the server's Retry-After), and at every start.
4. The outbox keeps at most 20 reports and none older than 7 days. The limits
   apply when a report is queued and again before every send, so a report older
   than 7 days is removed, never sent.

With the setting off, pending crashes are removed instead of queued, and turning
it off removes what was waiting, so a crash from that time is never sent later.
The setting is read again before each report is sent. A launcher started with
`--dry-launch` queues reports but sends nothing. The launcher, the game client
and `xerces_cli` read `launcher.json` the same way
(`xerces_core::crash_reports_enabled`): no file means the default, on; a file
that is not JSON means off until the switch is turned on again, since it may
have said off.

Games started by `xerces_cli` are covered too: `xerces_cli crash send` turns
pending crashes into reports and sends them (`--exit-code <code>` adds a crash
the client wrote nothing for); `xerces_cli crash list` shows the outbox and the
pending crashes. `crash send` refuses while Xerces.exe runs on the home, which
sends them itself, and exits 5 when reports still wait in the outbox for a later
try (no answer, rate limited).

## Contract (`xerces.crash/1`)

Base: `https://boiii-free-production.up.railway.app`. `XERCES_CRASH_URL`
replaces it, for local tests only: `http://` or `https://` to 127.0.0.1,
localhost or `[::1]`; anything else (another host, user info in the address)
sends nothing. The transport also refuses every host but the backend's and this
PC's.

### `GET {base}/v1/crashes`

Asked once before a run sends anything, with only the `User-Agent` header (no
install id, no body): does the server take crash reports? `404` means no (a
backend without crash routes, as the deployed one was when the beta went out):
the client sends nothing and asks again later. A `2xx`, or `405` (the path is
there for POST and DELETE only, which is what the backend on
`launcher/crash-backend` answers), means yes. Anything else, or no answer, is
asked again later, as a failed send is. The backend must not answer `404` here
once it takes reports.

### `POST {base}/v1/crashes`

Headers: `Content-Type: application/json`; `Content-Encoding: gzip` (optional;
the client gzips with zlib); `X-Xerces-Install: <32 lowercase hex>`;
`User-Agent: Xerces/<version>`.

Body: UTF-8 JSON, at most 4 MiB after gzip decoding.

```json
{
  "schema": "xerces.crash/1",
  "report_id": "<32 lowercase hex, random, made by the client; the idempotency key>",
  "created_utc": "<ISO 8601 UTC>",
  "xerces": {
    "version": "0.1.0-beta.1",
    "commit": "<git hash>",
    "dirty": false
  },
  "game": { "build": "0x6531394", "steam_build": 24784313 },
  "system": {
    "windows": "10.0.26300",
    "cpu": "<model name>",
    "logical_cpus": 24,
    "ram_mib": 65536,
    "gpus": [{ "name": "...", "driver": "32.0.15.8180" }],
    "display": "3840x2160@240"
  },
  "launch": {
    "mode": "zm|mp|cp",
    "map": "<map name or null>",
    "plan_id": "<or null>",
    "mods": [{ "id": "...", "name": "...", "version": "..." }],
    "flags": ["-xerces-db-pool"],
    "safe_mode": false
  },
  "crash": {
    "kind": "exception|fail_fast|out_of_memory|unknown_exit",
    "code": "0xC0000005",
    "exit_code": "<hex or null>",
    "module": "<module name or null>",
    "offset": "<hex or null>",
    "thread": "<main|server|auxiliary or null>",
    "frames": ["<module>+0x<hex>"],
    "phase": "<launching|symbols|frontend|loading_map|in_game|clean_exit>",
    "reason": "<exception|freeze|fatal_error or null>",
    "summary": "<what happened, one line, or null>",
    "script": "<the script that was running, one line, or null>"
  },
  "logs": {
    "journal": {},
    "game_log_tail": "<last <= 200 lines, redacted>",
    "launcher_log_tail": "<last <= 100 lines, redacted>",
    "console_tail": "<last <= 200 lines, redacted>",
    "details": "<the game's text report, its first <= 64 KiB, redacted, or null>"
  },
  "minidump_b64": "<base64 of the stacks-only minidump, <= 2 MiB raw, or null>"
}
```

- Strings are capped: names 256 bytes, each tail field 64 KiB, frames 32.
  Unknown fields are ignored; missing optional parts are `null`.
- `offset` and frames: for `BlackOps3.exe` the address in its image space
  (0x14...), as dumps and signatures name it; for other modules the offset from
  the module's base.
- Kinds: `exception` (the client's handler ran and wrote the small dump),
  `fail_fast` (exit 0xC0000409, no dump), `out_of_memory` (exit 0xFFFFFFFF after
  the out-of-memory box), `unknown_exit` (any other end that was not clean, a
  fatal engine error among them).
- `reason`, `summary`, `script` and `details` were added on 2026-11-12
  (beta.22) without a new schema name: a backend from before ignores them, as
  it does every field it does not know, and a client from before sends none. A
  report for a crash the client wrote no note for has `null` for all four. The
  backend stores them from the commit "Backend: the game's text report in crash
  reports" on (`launcher/friends-backend`); until that runs on Railway the
  summary still reaches the developer, as the console tail's last lines.
- `tests/fixtures/crash/report_golden.json` is a full report the client's
  builder must produce field for field (`xerces_tests crash`).

Answers: `201 {"id": "<server id>", "duplicate": false}`;
`200 {"id": "...", "duplicate": true}` for a known `report_id`; `400` invalid;
`413` too large (the client tries once more without the minidump); `429` rate
limited, with `Retry-After`. The client removes a report on 200 and 201, and on
400, 413 and 422 (it would never be taken); it keeps it for later on anything
else.

### `DELETE {base}/v1/crashes`

Header `X-Xerces-Install`. Deletes every report of that install:
`200 {"deleted": n}`. Settings > Privacy > "Delete my crash reports" calls it.

### Admin (the developer only)

`Authorization: Bearer <XERCES_ADMIN_TOKEN>` (a server environment variable,
compared in constant time; unset, the admin routes answer 404):

- `GET /v1/admin/crashes?since=<iso>&signature=<hex>&version=<v>&limit=<<=200>&cursor=<opaque>`:
  newest first, metadata only, with the report's `reason` and `summary` when
  the game wrote them (`scripts/crashes.py list` prints the summary under each
  report, and `show` prints the game's text report before the logs).
- `GET /v1/admin/crashes/{id}`: the full report without the minidump's bytes,
  plus `has_minidump`.
- `GET /v1/admin/crashes/{id}/minidump`: `application/octet-stream`.
- `GET /v1/admin/crash-groups?since=<iso>`: per signature: count, installs,
  first and last seen, versions, top mods, top maps, a sample id.

Signature: the first 16 hex digits of the SHA-256 of kind, module, offset and
the first 3 frames. Retention: the backend's periodic task deletes reports older
than 90 days.
