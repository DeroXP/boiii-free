# Crash reports

Decided on 2026-11-05 (D21, DESIGN.md 10.5 and 5.10): when the game crashes,
Xerces sends a crash report to the Xerces backend on Railway, where the
developer studies it. Only while the launcher setting "Send crash reports" is
on; it is on by default. The first run's privacy step and Settings > Privacy say
what is sent and have the switch. With it off nothing is sent, and testers
report crashes at <https://github.com/DeroXP/boiii-free/issues> with the
diagnostics bundle (Xerces > Diagnostics > Copy report).

The client and the backend implement this contract exactly. The backend side is
`backend/` (branch `xerces-design`); the client side is
`src/xerces_core/crash_report.*` and `crash_outbox.*`, the game's crash handler
(`src/client/component/exception.cpp`), the launcher (`services.cpp`, Settings,
first run) and `xerces_cli crash list|send`.

## What is sent

- The crash: its kind, code and exit code, the module and offset it happened at,
  the thread (main, server, auxiliary), up to 32 frames as `module+0xoffset`,
  and the launch journal's phase.
- A stacks-only minidump (`MiniDumpNormal | MiniDumpWithThreadInfo`): the
  threads, their registers and stacks. No data segments and no memory the stacks
  point to, which can hold tokens and other players' addresses. At most 2 MiB;
  larger ones are left out.
- The launch: game mode, map, plan id, the mods (id, name, version), the flags
  (names only), safe mode.
- Versions: Xerces' version, git commit and whether the tree was dirty; the
  game's exe checksum and Steam build.
- This PC: Windows version, CPU name, logical CPUs, RAM, GPUs with driver
  versions, the primary display's mode.
- Logs: the launch journal, and the last lines of the game's log (200), the
  launcher's log (100) and the console (200), each at most 64 KiB.

Before anything leaves the PC the client removes: the profile folder (written
`%USERPROFILE%`), any other `X:\Users\<name>` (written `C:\Users\<user>`), the
Windows user name, the player name, Steam ids, values after `xuid`, server
addresses and favourites' names (the diagnostics bundle's redaction,
`xerces_core/log.hpp`, plus these). The server checks again that no
`C:\Users\<name>` path survives.

Never sent: usage data or analytics, the player name, Steam ids or the game
identity (XUID), full crash dumps, settings, packs or files. The full dumps stay
in `%LOCALAPPDATA%\boiii\minidumps` for local use.

Reports are tied to an install id: 32 random hex digits from the system's random
source in `<home>\config\install_id`, made the first time a report is sent,
never derived from hardware or accounts. "Delete my crash reports" deletes every
report of that id. The server keeps reports 90 days and stores no IP address (it
uses the address in memory for rate limits only).

## How a crash becomes a report

1. The game's crash handler, when the launcher started the game (`XERCES_HOME`
   is set) and the setting is on, writes `<home>\crash\pending\<report_id>.dmp`
   (the stacks-only dump) and `<report_id>.json` next to it (code, module,
   offset, thread, frames, phase, the console's last lines, the flags, the game
   log's file name and the journal). Upstream's own dump and zip stay as they
   are. The dialog says the report is sent when the game closes (the launcher is
   open) or when Xerces opens next; with the setting off it points to GitHub and
   the bundle.
2. When the game the launcher started exits, the launcher counts it as a crash
   when the client wrote a dump, or the exit code is not 0 and the journal does
   not end in `clean_exit` (`xerces_core::exit_is_crash`). Each pending crash
   becomes a report in `<home>\crash\outbox\<report_id>.json`; a crash without
   one gets a report from the exit code (`fail_fast` for 0xC0000409,
   `out_of_memory` for 0xFFFFFFFF after the out-of-memory box, else
   `unknown_exit`), once per launch.
3. A worker thread sends the outbox, oldest first: never on the UI thread, never
   holding up a launch, each step timing out after 15 s. A notice says "Crash
   report sent" with the server's id, or that it is sent later. A failed send is
   tried again in 10 minutes while Xerces is open (or after the server's
   Retry-After), and at every start.
4. The outbox keeps at most 20 reports and none older than 7 days.

With the setting off, pending crashes are removed instead of queued, and turning
it off removes what was waiting, so a crash from that time is never sent later.
The setting is read again before each report is sent. A launcher started with
`--dry-launch` queues reports but sends nothing.

Games started by `xerces_cli` are covered too: `xerces_cli crash send` turns
pending crashes into reports and sends them (`--exit-code <code>` adds a crash
the client wrote nothing for); `xerces_cli crash list` shows the outbox and the
pending crashes. `crash send` refuses while Xerces.exe runs on the home, which
sends them itself.

## Contract (`xerces.crash/1`)

Base: `https://boiii-free-production.up.railway.app`. `XERCES_CRASH_URL`
replaces it, for local tests only: `https://`, or `http://` to 127.0.0.1,
localhost or `[::1]`; anything else sends nothing.

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
    "phase": "<launching|symbols|frontend|loading_map|in_game|clean_exit>"
  },
  "logs": {
    "journal": {},
    "game_log_tail": "<last <= 200 lines, redacted>",
    "launcher_log_tail": "<last <= 100 lines, redacted>",
    "console_tail": "<last <= 200 lines, redacted>"
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
  the out-of-memory box), `unknown_exit` (any other end that was not clean).
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
  newest first, metadata only.
- `GET /v1/admin/crashes/{id}`: the full report without the minidump's bytes,
  plus `has_minidump`.
- `GET /v1/admin/crashes/{id}/minidump`: `application/octet-stream`.
- `GET /v1/admin/crash-groups?since=<iso>`: per signature: count, installs,
  first and last seen, versions, top mods, top maps, a sample id.

Signature: the first 16 hex digits of the SHA-256 of kind, module, offset and
the first 3 frames. Retention: the backend's periodic task deletes reports older
than 90 days.
