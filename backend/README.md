# Xerces backend

The online half of Xerces: accounts, friends, presence, game invites and short
pack share codes for the launcher, and the crash reports testers' launchers
send. Self-hosted and offline play never need it. It is a small FastAPI service
on PostgreSQL, built to run as one replica on Railway.

## What it does

- Accounts: a username, an Argon2id password hash and an optional email that is
  never shown to anyone else and never sent anything. Access tokens are EdDSA
  JWTs that live 10 minutes; refresh tokens live 30 days and rotate on every
  use, with reuse detection.
- Friends: requests, blocks and reports, with privacy settings for who can send
  requests, see presence and send invites.
- Presence: one WebSocket per user carries online status, what the user is
  playing and live pushes.
- Invites: game invites between friends that expire after 10 minutes.
- Pack share codes: `XRC-` plus 8 characters for a validated pack (64 KB at
  most, no local file paths), kept 90 days.
- Your data: export everything stored about you, or delete the account at once.
- Crash reports: a launcher with "Send crash reports" on sends one after a game
  it started crashes. They are keyed to a random install id, never to an
  account, kept 90 days, and read by the project owner with
  `scripts/crashes.py`. See [Crash reports](#crash-reports).

## What it does not do yet

- No game server sessions, relay (TURN) credentials or server list. Those arrive
  with milestone M6.
- No email sending, password reset by email, moderation tools for reports or
  Discord integration.
- No horizontal scaling. Presence, the WebSocket hub and rate limits live in
  memory, so the service runs as exactly one replica.

## API

Bodies are JSON. Signed-in routes take `Authorization: Bearer <access token>`.
Outside production the interactive docs are served at `/docs`.

| Method | Path                                | Auth        | Result                                                       |
| ------ | ----------------------------------- | ----------- | ------------------------------------------------------------ |
| GET    | `/healthz`                          | no          | `{"ok": true}` after `SELECT 1`, 503 if the database is down |
| GET    | `/v1/meta`                          | no          | service, version, API level, server time                     |
| POST   | `/v1/auth/register`                 | no          | 201, user and token pair                                     |
| POST   | `/v1/auth/login`                    | no          | user and token pair                                          |
| POST   | `/v1/auth/refresh`                  | no          | new token pair; the old refresh token is spent               |
| POST   | `/v1/auth/logout`                   | no          | 204, revokes the refresh token's family, closes the socket   |
| GET    | `/v1/me`                            | yes         | your account                                                 |
| PATCH  | `/v1/me/privacy`                    | yes         | set `friend_requests`, `presence`, `invites`                 |
| POST   | `/v1/me/password`                   | yes         | 204, signs out every session and access token                |
| GET    | `/v1/me/export`                     | yes         | everything stored about you                                  |
| DELETE | `/v1/me`                            | yes         | 204, body `{"password"}`, deletes the account                |
| GET    | `/v1/friends`                       | yes         | friends, with presence where allowed                         |
| POST   | `/v1/friends/requests`              | yes         | always 202 `{"status": "sent"}`                              |
| GET    | `/v1/friends/requests`              | yes         | incoming and outgoing requests                               |
| POST   | `/v1/friends/requests/{id}/accept`  | yes         | recipient only                                               |
| POST   | `/v1/friends/requests/{id}/decline` | yes         | recipient only, 204                                          |
| DELETE | `/v1/friends/requests/{id}`         | yes         | sender cancels, 204                                          |
| DELETE | `/v1/friends/{user_id}`             | yes         | unfriend, 204                                                |
| GET    | `/v1/blocks`                        | yes         | who you blocked                                              |
| POST   | `/v1/blocks`                        | yes         | 204, body `{"username"}`                                     |
| DELETE | `/v1/blocks/{user_id}`              | yes         | unblock, 204                                                 |
| POST   | `/v1/reports`                       | yes         | always 202                                                   |
| POST   | `/v1/invites`                       | yes         | 201 between friends, otherwise 403                           |
| GET    | `/v1/invites`                       | yes         | pending incoming, and your outgoing                          |
| POST   | `/v1/invites/{id}/accept`           | yes         | recipient only                                               |
| POST   | `/v1/invites/{id}/decline`          | yes         | recipient only, 204                                          |
| POST   | `/v1/packs`                         | yes         | 201, `{"code", "expires_at"}`                                |
| GET    | `/v1/packs/{code}`                  | no          | the pack, or 404                                             |
| DELETE | `/v1/packs/{code}`                  | yes         | owner only, 204                                              |
| WS     | `/v1/ws`                            | first frame | presence and pushes                                          |
| POST   | `/v1/crashes`                       | install id  | 201 `{"id", "duplicate": false}`, 200 for a resent report    |
| DELETE | `/v1/crashes`                       | install id  | `{"deleted": n}`: every report of that install               |
| GET    | `/v1/admin/crashes`                 | admin token | reports, newest first, metadata only                         |
| GET    | `/v1/admin/crashes/{id}`            | admin token | one report in full, without the minidump's bytes             |
| GET    | `/v1/admin/crashes/{id}/minidump`   | admin token | the minidump, `application/octet-stream`                     |
| GET    | `/v1/admin/crash-groups`            | admin token | reports grouped by signature                                 |

Every error has the same shape, `{"error": {"code": "...", "message": "..."}}`.
Friend requests, blocks and reports answer the same way whether or not the user
exists, accepts requests or has blocked you. A request to someone who blocked
you or accepts no requests is kept for you alone: it sits in your outgoing list
like an unanswered request until it expires, and they never see it. Sending it
again after they start accepting requests from you delivers it, and if they ask
you themselves, you become friends, as with any request you sent.

## WebSocket

Connect to `wss://<your-domain>/v1/ws` and send the access token as the first
frame, within 10 seconds. Tokens in the URL are never accepted.

```text
client: {"type": "auth", "token": "<access token>"}
server: {"type": "hello", "user_id": "...", "friends": [{"user_id": "...", "status": "online", "activity": null}]}
client: {"type": "presence", "status": "in_game", "activity": {"mode": "zm", "joinable": true, "pack_code": "XRC-AB12CD34"}}
client: {"type": "ping"}
server: {"type": "pong"}
client: {"type": "auth", "token": "<fresh access token>"}
server: {"type": "auth_ok"}
```

Status is `online`, `away` or `in_game`, and `mode` is `zm`, `mp`, `cp` or null.
A presence that repeats the current one is not passed on. An account may send 10
messages a second, counted across its connections, and sign in 10 times a
minute. A new connection for the same account closes the old one.

The socket lives only as long as the access token it was opened with. To keep it
open, send each fresh access token over it, as above, before the old one
expires; otherwise it closes with 4401. Logging out, a refresh token being used
twice (which revokes its family) and a password change also close it with 4401,
a socket that was signing in at that moment included, and after a password
change only tokens issued since then can reconnect.

The server pushes:

- `presence` with `user_id`, `status` and `activity`, to friends allowed to see
  it
- `offline` with `user_id` when a friend disconnects, or stops being visible
- `invite` with the invite's fields when a friend invites you
- `friend_request` when someone sends you a friend request
- `friend_added` with `user_id` and `username` when a request is accepted
- `error` with `code` and `message` for a malformed message (the connection
  stays open)

| Close code | Meaning                                                                            |
| ---------- | ---------------------------------------------------------------------------------- |
| 1001, 1012 | Server restarting. Reconnect.                                                      |
| 1003       | Binary frame. Only JSON text frames are accepted.                                  |
| 1008       | First frame was not auth, auth took over 10 seconds, or over a rate limit.         |
| 1009       | Message too big.                                                                   |
| 1011       | Server error. Reconnect after a short wait.                                        |
| 1013       | The client stopped reading its messages.                                           |
| 4000       | Replaced by a newer connection for the same account.                               |
| 4401       | Token invalid or expired, signed out or password changed. Refresh, then reconnect. |
| 4410       | Account deleted.                                                                   |

## Crash reports

The launcher and the backend share one contract, `xerces.crash/1`. The launcher
sends a report when a game it started ends in a crash and its "Send crash
reports" setting is on (it is on by default; with it off nothing is sent). Full
crash dumps stay on the PC; a report carries at most a stacks-only minidump.

```text
POST /v1/crashes
Content-Type: application/json
Content-Encoding: gzip                 (optional)
X-Xerces-Install: <32 lower-case hex>  (random, kept in <home>\config\install_id)
User-Agent: Xerces/<version>
```

The body is one JSON object: `schema` (`"xerces.crash/1"`), `report_id` (32
lower-case hex, made by the launcher), `created_utc`, `xerces` (`version`,
`commit`, `dirty`), `game` (`build`, `steam_build`), `system` (`windows`, `cpu`,
`logical_cpus`, `ram_mib`, `gpus`, `display`), `launch` (`mode`, `map`,
`plan_id`, `mods`, `flags`, `safe_mode`), `crash` (`kind`, `code`, `exit_code`,
`module`, `offset`, `thread`, `frames`, `phase`), `logs` (`journal`,
`game_log_tail`, `launcher_log_tail`, `console_tail`) and `minidump_b64`. Only
`schema`, `report_id`, `created_utc`, `xerces.version` and `crash.kind` are
required; any other part may be missing or null, and unknown fields are ignored
(and not stored). `crash.kind` is `exception`, `fail_fast`, `out_of_memory` or
`unknown_exit`; `code`, `exit_code` and `offset` are hex numbers written
`0x...`.

| Answer | When                                                                                     |
| ------ | ---------------------------------------------------------------------------------------- |
| 201    | stored: `{"id": "<server id>", "duplicate": false}`                                      |
| 200    | a `report_id` the server already has: `{"id": "<its server id>", "duplicate": true}`     |
| 400    | not valid: bad JSON, a missing or wrong field, a missing or malformed `X-Xerces-Install` |
| 408    | the body took over 30 seconds to arrive                                                  |
| 413    | the body is over 4 MiB (after gzip decoding too), or the minidump over 2 MiB             |
| 415    | not `application/json` in UTF-8, or a `Content-Encoding` other than gzip                 |
| 429    | over a rate limit; `Retry-After` says when to try again                                  |
| 503    | 8 big uploads are already being read; try again after `Retry-After`                      |

Limits: names and other short strings 256 characters (versions, hashes and
phases 64), each log tail 64 KiB, at most 32 frames, 512 mods, 64 flags and 16
GPUs, the journal 256 KB and 32 levels deep. Sending the same `report_id` again
never stores a second copy, also when two copies arrive at once, so a launcher
can retry until it gets an answer. Every report counts against the limits per
install and per address, rejected and repeated ones too; only stored ones count
against the limit for all senders together.

Privacy:

- The launcher redacts before sending (profile paths, user names, server
  addresses, favourite names; no player name, Steam id or XUID). The server
  checks again: in every string, keys included, a Windows profile path such as
  `C:\Users\Bob\...` becomes `C:\Users\<user>\...`, and `/home/<name>` (Proton)
  becomes `/home/<user>`. `Public` and `Default` stay. In the minidump, which is
  binary, each such name's characters are overwritten with `_` in place, so its
  offsets stay valid. Debuggers find modules by file name, size and time stamp,
  so this costs nothing; point the symbol path at the release PDBs.
- No IP address is stored or logged. It is used only in memory, as an HMAC, for
  the per-address rate limits. The user agent and the install id are not logged
  either, and the user agent is not stored.
- `DELETE /v1/crashes` with the same `X-Xerces-Install` header deletes every
  report of that install, minidumps included, and answers `{"deleted": n}`. The
  launcher offers it as "Delete my crash reports".
- Reports are deleted 90 days after the server received them.

Each report gets a signature that groups the same crash: the first 16 hex
characters of the SHA-256 of `kind`, `module`, `offset` and the first three
`frames`, each lower-cased with leading zeros dropped from hex numbers (`0x0010`
counts as `0x10`), joined with `|`; missing parts count as empty. For example
`exception|blackops3.exe|0x1a2b|blackops3.exe+0x1a2b|boiii.exe+0x20|ntdll.dll+0x30`.

### Reading crash reports

The admin routes are for the project owner only. They need
`Authorization: Bearer <XERCES_ADMIN_TOKEN>`; while that variable is unset on
the server they answer 404 like any unknown path. Tokens are compared in
constant time, and 10 wrong ones from one address in 15 minutes lock that
address out for a while.

- `GET /v1/admin/crashes`: newest first, metadata only, as
  `{"items": [...], "next_cursor": ...}`. Filters: `since` (an ISO time),
  `signature`, `version`; `limit` 1 to 200 (50 by default); pass `next_cursor`
  back as `cursor` for the next page.
- `GET /v1/admin/crashes/{id}`: the whole report as stored, with `has_minidump`
  and `minidump_size` but without the minidump.
- `GET /v1/admin/crashes/{id}/minidump`: the minidump file.
- `GET /v1/admin/crash-groups`: per signature the count, installs, first and
  last time seen, versions, top 5 mods and maps and the newest report's id, most
  frequent first. Takes `since` and `limit` (1 to 500, 100 by default).

`scripts/crashes.py` reads them for you. It needs only Python, no packages. Set
the two variables in the shell first; the token is the one on Railway:

```text
cd backend
set XERCES_BACKEND_URL=https://boiii-free-production.up.railway.app
set XERCES_ADMIN_TOKEN=<the value of XERCES_ADMIN_TOKEN on Railway>
python scripts/crashes.py groups --since 7d
python scripts/crashes.py list --signature <signature> --all
python scripts/crashes.py show <id>
python scripts/crashes.py minidump <id>
```

`groups` is where to start: the most frequent crashes first, with a sample
report id. `show` prints that report with its logs; `--json` prints the raw
JSON. `minidump` saves `<id>.dmp` in the current folder for WinDbg or Visual
Studio. The script sends the token only over HTTPS (plain HTTP only to
`127.0.0.1` or `localhost`, for local tests).

## Rate limits

Counted in memory, per account, or per client address where nobody is signed in.
Answers over a limit are 429 with `Retry-After` in seconds. Addresses are only
used as an HMAC keyed with `XERCES_RATE_LIMIT_SECRET` and are never stored or
logged.

| What                       | Limit                                                          |
| -------------------------- | -------------------------------------------------------------- |
| Registration               | 3 per hour per address, a taken name included                  |
| Login                      | 10 per 15 minutes per address                                  |
| Failed logins              | 5 per 15 minutes per username and address, 50 per username     |
| Token refresh              | 30 per minute per account, and per address for unusable tokens |
| Friend requests            | 20 per day per account                                         |
| Invites                    | 30 per hour per account, and 1 per recipient per 30 seconds    |
| Reports                    | 10 per day per account                                         |
| Pack creation              | 20 per day per account                                         |
| Pack lookup                | 120 per minute per address                                     |
| Wrong password on `/v1/me` | 5 per 15 minutes per account                                   |
| WebSocket sign-ins         | 10 per minute per account, and per address for those that fail |
| WebSocket messages         | 10 per second per account, then close 1008                     |
| Crash reports              | 20 per hour per install id, 60 per hour per address            |
| Crash reports, all senders | 2000 stored per day together, so a flood cannot fill the disk  |
| Deleting crash reports     | 10 per hour per address                                        |
| Wrong admin tokens         | 10 per 15 minutes per address                                  |

Login attempts and password checks are counted before the password is hashed and
given back unless it was wrong, so successful logins and attempts that end in a
server error do not count, and a burst of parallel guesses gets no more tries
than a slow one.

Failed logins count per username and address, and per username from every
address together. So guessing at one account is capped at 50 per 15 minutes
however many addresses are used, and locking its owner out takes at least ten
addresses at once, since one address stops after its own five. Launchers that
are already signed in are unaffected either way, because they refresh their
tokens instead of logging in.

Sign-up, login and the password checks on `/v1/me` hash the password, at most 4
at a time with up to 64 more requests waiting. Past that they answer 503 with
`Retry-After` straight away, which counts against no limit. No request holds a
database connection while it waits for a hash, so a flood of made-up logins can
slow sign-ins but nothing else.

Each limit keeps track of at most 100,000 keys. A key with its full allowance
left is the same as a new one and is forgotten first; if that frees no room, new
keys for that limit get 429 until old ones refill, so a flood of new addresses
cannot reset a limit that is holding someone back.

On Railway the address comes from `X-Forwarded-For` (this needs
`XERCES_TRUST_PROXY=1`). Railway's edge drops any `X-Forwarded-For` a client
sends and writes its own, with the client's address first and a Railway hop
after it, so by default the first entry counts. If the edge ever appends to the
client's header instead, set `XERCES_PROXY_HOPS=2` to count the second entry
from the right (see the check under Running it). The per-account and
per-username limits do not depend on any of this.

## Security and privacy

- Passwords: Argon2id at the OWASP minimum by default (19 MiB, 2 iterations, 1
  lane), hashed in a thread pool at most 4 at a time, and rehashed at login when
  the parameters change. A login with an unknown username still spends one hash,
  so timing does not reveal accounts.
- Usernames are 3 to 24 characters from A-Z, digits, `_`, `.` and `-` after NFKC
  normalization, unique ignoring case. Names such as `admin` and `xerces` are
  reserved. Passwords are 10 to 128 characters and may not equal the username.
- Access tokens carry a `kid` header and are checked for issuer `xerces`,
  audience `xerces-client` and type `access`; only EdDSA is accepted.
- Refresh tokens are 256-bit random values stored only as SHA-256. Each refresh
  replaces the token; presenting a replaced token revokes its whole family (RFC
  9700). Logout revokes the family, and a password change or account deletion
  revokes them all. A password change also ends every access token issued before
  it, so a copied token stops working at once rather than after its 10 minutes.
  Changes to one account's sessions take turns (a row lock), so a revocation
  never misses a token issued at the same moment, and a login or password change
  that checked the old password fails once a new one is set.
- Request bodies over 128 KB are refused before parsing, except crash reports,
  which may be 4 MiB (8 read at a time, each within 30 seconds). Responses are
  marked as never to be cached (`no-store`) or sniffed (`nosniff`). There is no
  CORS, since the launcher is a desktop client.
- Stored: username, password hash, optional email, privacy settings, the time of
  the last password change, friends, pending requests, blocks, reports, invites
  and packs, and for 90 days the names of deleted accounts and crash reports
  (keyed to an install id, not an account). Never stored or logged: IP
  addresses, user agents and tokens. Presence lives in memory only.
- Logs are JSON lines on stdout with event names, and user ids on account events
  such as logins. They never contain tokens, passwords, emails or addresses.
  Sockets are logged with a connection count only, so the logs keep nobody's
  online history, and an account deletion is logged without its id.
- `GET /v1/me/export` returns every row about you. Reports other people filed
  about you appear with their reason, status and day, but not who filed them or
  what they wrote. Incoming requests that were never shown to you (sent while
  you had blocked the sender or accepted no requests) are marked `hidden`.
- `DELETE /v1/me` removes the account and everything keyed to it at once.
  Reports keep the row but lose the link to the deleted account. The username
  stays taken for 90 days, to the end of that day, so nobody can register it
  straight away and pass as the person who left. It is kept with no link to the
  account: written on its own just before the deletion, with only the day it
  ends.
- Every 10 minutes a background task deletes expired invites, friend requests,
  refresh tokens, packs and held usernames, and reports and crash reports older
  than 90 days.

## Local development

Needs Python 3.12 or newer. Keep the virtual environment outside the repository.
With `XERCES_ENV=development` and no secrets set, the app makes throwaway keys
and says so in the log. Data goes to a local SQLite file named `xerces-dev.db`.

```text
cd backend
python -m venv C:\dev\xerces-venv
C:\dev\xerces-venv\Scripts\python -m pip install -r requirements.txt -r requirements-dev.txt
copy .env.example .env
C:\dev\xerces-venv\Scripts\python -m alembic upgrade head
C:\dev\xerces-venv\Scripts\python -m uvicorn app.main:app --reload
```

Then open `http://127.0.0.1:8000/docs` to try the API.

## Tests

```text
cd backend
C:\dev\xerces-venv\Scripts\python -m pytest -q
```

The tests use SQLite through aiosqlite, run the migrations against a temporary
database and drive the WebSocket in-process, so they need no network and no
PostgreSQL. Dependencies are pinned in `requirements.txt` (runtime, installed in
the image) and `requirements-dev.txt` (tests). Every pin installs from wheels on
CPython 3.12, the image's Python, and on 3.14.

## Database migrations

Alembic with an async engine. The container runs `alembic upgrade head` before
it starts the server, so a deploy applies new migrations by itself. After
changing `app/models.py`, generate a migration, read it, and commit it:

```text
python -m alembic revision --autogenerate -m "describe the change"
```

`tests/test_migrations.py` fails when the migrations and the models disagree.

Rolling a deploy back across a migration needs a manual downgrade first. The old
image's `start.sh` runs `alembic upgrade head`, which fails on a revision that
image does not know, so the old deployment would never start. While the new
deployment still runs, open a shell in it (`railway ssh`) and run
`alembic downgrade <revision>` with the head revision of the image you go back
to (`0001` to undo `0002`, `0002` to undo `0003`), then roll back. Downgrading
0002 deletes the friend requests that were hidden from their recipients, since
0001 could only show them, and keeps the names held for deleted accounts for
when you upgrade again. Downgrading 0003 drops the crash report tables, and
every crash report with them.

## Environment variables

| Variable                         | On Railway     | Default                    | Purpose                                                                     |
| -------------------------------- | -------------- | -------------------------- | --------------------------------------------------------------------------- |
| `DATABASE_URL`                   | required       | SQLite file in development | PostgreSQL URL; `postgres://` and `postgresql://` are converted for asyncpg |
| `XERCES_ENV`                     | required       | `production`               | `production`, `development` or `test`                                       |
| `XERCES_TOKEN_PRIVATE_KEY`       | required       | throwaway in development   | Ed25519 signing key, a base64url 32-byte seed or PEM                        |
| `XERCES_TOKEN_KEY_ID`            | required       | throwaway in development   | key id written into each token header                                       |
| `XERCES_TOKEN_EXTRA_PUBLIC_KEYS` | optional       | none                       | JSON object of old key ids to PEM public keys, accepted during a rotation   |
| `XERCES_RATE_LIMIT_SECRET`       | required       | throwaway in development   | at least 32 characters; keys the HMAC of client addresses                   |
| `XERCES_ADMIN_TOKEN`             | for crashes    | unset: admin routes 404    | at least 32 characters; the bearer token of the `/v1/admin` routes          |
| `XERCES_TRUST_PROXY`             | `1`            | `0`                        | take the client address from `X-Forwarded-For` (uvicorn trusts it too)      |
| `XERCES_PROXY_HOPS`              | optional       | `0`                        | `0`: first `X-Forwarded-For` entry; `N`: the Nth from the right             |
| `XERCES_ARGON2_TIME_COST`        | optional       | `2`                        | Argon2id iterations                                                         |
| `XERCES_ARGON2_MEMORY_KIB`       | optional       | `19456`                    | Argon2id memory per hash, in KiB                                            |
| `XERCES_ARGON2_PARALLELISM`      | optional       | `1`                        | Argon2id lanes                                                              |
| `PORT`                           | set by Railway | `8080`                     | port uvicorn listens on                                                     |

In production the service refuses to start when the signing key, key id or
rate-limit secret is missing, when `DATABASE_URL` is missing or points at
SQLite, or when `XERCES_ADMIN_TOKEN` is set but shorter than 32 characters.

`DATABASE_URL` can hold the password as Railway writes it: the app encodes `@`,
`:`, `/`, `?` and `#` in the user name and password itself. Only a `%` must be
written as `%25`. Errors about the URL name the variable and never repeat its
value.

Leave `XERCES_TRUST_PROXY` unset (or `0`) anywhere a client can reach the
container without passing Railway's edge. Then `start.sh` also starts uvicorn
with `--no-proxy-headers`, so a client cannot choose the address its rate limits
are counted against by sending its own `X-Forwarded-For`.

## Deploying on Railway

Railway's config file (`railway.json`) is deprecated and new services cannot use
it, so these settings are made in the dashboard.

1. In Railway, create a new project and choose Deploy from GitHub repo. Pick
   this repository and the branch that contains `backend/`. If Railway starts
   building the repository root before step 2, let that build fail.
2. Open the new service. In its Settings, under Source, set the Root Directory
   to `backend`. Railway then builds `backend/Dockerfile`.
3. Still in Settings, set the Healthcheck Path to `/healthz` and the restart
   policy to On Failure, and keep one replica.
4. Add the database: on the project canvas choose Create, then Database, then
   PostgreSQL.
5. In the backend service, open Variables and add `DATABASE_URL`. As its value,
   enter the reference `${{Postgres.DATABASE_URL}}`, which points at the
   database's private URL.
6. On your PC, in the `backend` folder, run `python scripts/genkey.py`. Paste
   the three lines it prints into the service's variables with the Raw Editor.
   Do not save them anywhere else.
7. Add `XERCES_ENV=production` and `XERCES_TRUST_PROXY=1`. To read crash
   reports, also run `python scripts/genkey.py --admin` and add the line it
   prints (`XERCES_ADMIN_TOKEN=...`); keep a copy for `scripts/crashes.py`, for
   example in your password manager, and nowhere else. Then deploy. The
   container runs the migrations and starts the server; Railway waits for the
   health check to pass before it sends traffic to the new deployment.
8. In Settings, under Networking, choose Generate Domain. If Railway asks for a
   port, give the one in the deploy log's startup line (8080 unless `PORT` is
   set).
9. Open `https://<your-domain>/healthz`. It should answer `{"ok": true}`. The
   path `/v1/meta` shows the version.
10. In the workspace settings under Usage, set a soft limit (an email alert) and
    a hard limit with a margin above normal use. Hitting the hard limit stops
    every service, the database included.

## Running it

- Keep exactly one replica. Presence, the WebSocket hub and rate limits are in
  memory, and a second replica would split them.
- Railway reported a security incident from 2026-10-20 to 22 that exposed some
  environment variables. Rotate anything created before 2026-10-21: generate the
  three backend secrets again with `genkey.py`, and change the PostgreSQL
  password.
- Rotating the signing key signs nobody out, because refresh tokens are random
  values in the database and clients simply refresh. To avoid even one rejected
  request, keep the old public key in `XERCES_TOKEN_EXTRA_PUBLIC_KEYS` for 10
  minutes, written as `{"<old key id>": "<PEM>"}`. To print the old key's PEM,
  run `python scripts/genkey.py --public -` and paste its seed: read from the
  input, the seed stays out of your shell history.
- Back up the database: Railway's native backups on the Pro plan, or a cron
  service that runs `pg_dump` to encrypted storage outside Railway. Test a
  restore now and then, and keep retention at 30 days or less, or state it in
  the privacy policy.
- After the first deploy, check that a client cannot pick its own address: send
  11 logins with made-up usernames, each with a different made-up
  `X-Forwarded-For`. The 11th should be refused with 429. If it is not,
  Railway's edge now passes the client's header on: set `XERCES_PROXY_HOPS=2`,
  redeploy and check again. Then check from a second network (a phone off Wi-Fi)
  that a login there is not refused, which would mean every client shares one
  address. The per-account and per-username limits hold either way.
- Point an external uptime check at `/healthz`. Logs are JSON lines, so the
  Railway log view can filter them by `event`.
- There is no moderation UI yet. Reports stay 90 days in the `reports` table.
- Crash reports: start with `python scripts/crashes.py groups --since 7d` (see
  [Reading crash reports](#reading-crash-reports)). To change the admin token,
  run `genkey.py --admin` again and replace the variable on Railway and in your
  shell; the old token stops working with the redeploy that follows.

## Licence

MIT, see `LICENSE`. This folder is licensed on its own; the rest of the
repository is GPL-3.0.
