"""POST and DELETE /v1/crashes: storage, idempotency, validation, size limits and rate limits."""

import asyncio
import base64
import gzip
import hashlib
import json
import logging
import time
import tracemalloc
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import select

from app.crashes import crash_signature, prepare_body
from app.models import CrashMinidump, CrashReport
from app.ratelimit import DEFAULT_RULES, HOUR, Rule
from app.schemas import CRASH_REST_MAX
from tests.crashdata import (
    INSTALL,
    crash_report,
    minidump,
    minimal_report,
    new_hex_id,
    post_crash,
)
from tests.helpers import (
    RELAXED_RULES,
    AppFactory,
    client_for,
    make_settings,
    services_of,
    strict_rules,
)

MIB = 1024 * 1024


async def _rows(app: FastAPI) -> list[CrashReport]:
    async with services_of(app).sessionmaker() as db:
        return list((await db.scalars(select(CrashReport))).all())


async def _dumps(app: FastAPI) -> dict[Any, bytes]:
    async with services_of(app).sessionmaker() as db:
        rows = (await db.execute(select(CrashMinidump.crash_id, CrashMinidump.data))).all()
    return {row.crash_id: row.data for row in rows}


def _error(response: httpx.Response) -> dict[str, str]:
    return response.json()["error"]


async def test_a_report_is_stored(client: httpx.AsyncClient, app: FastAPI) -> None:
    report = crash_report()
    response = await post_crash(client, report)
    assert response.status_code == 201, response.text
    body = response.json()
    assert set(body) == {"id", "duplicate"} and body["duplicate"] is False

    [row] = await _rows(app)
    assert str(row.id) == body["id"]
    assert row.report_id == report["report_id"]
    assert row.install_id == INSTALL
    assert row.created_utc.isoformat() == "2026-11-05T12:00:00+00:00"
    assert (row.version, row.commit, row.dirty) == ("0.1.0-beta.1", "a32259cb", False)
    assert (row.game_build, row.steam_build) == ("0x6531394", 24784313)
    assert (row.kind, row.code, row.module, row.offset) == (
        "exception",
        "0xC0000005",
        "BlackOps3.exe",
        "0x1a2b3c",
    )
    assert (row.phase, row.mode, row.map, row.plan_id, row.safe_mode) == (
        "in_game",
        "zm",
        "zm_castle",
        "plan-7",
        False,
    )
    assert row.flags == ["-xerces-db-pool"]
    assert row.mods == report["launch"]["mods"]
    assert row.system == report["system"]
    assert row.crash == report["crash"]
    assert row.logs == report["logs"]
    assert row.signature == crash_signature(
        "exception", "BlackOps3.exe", "0x1a2b3c", report["crash"]["frames"]
    )
    dumps = await _dumps(app)
    assert dumps == {row.id: minidump(b"stack")}
    # What the storage cap counts: the minidump plus the rest as the database holds it.
    assert row.stored_bytes == prepare_body(json.dumps(report).encode()).stored_bytes
    assert len(minidump(b"stack")) + len(json.dumps(report["logs"])) < row.stored_bytes


async def test_gzip_bodies_are_accepted(client: httpx.AsyncClient, app: FastAPI) -> None:
    response = await post_crash(client, crash_report(), compress=True)
    assert response.status_code == 201, response.text
    assert len(await _rows(app)) == 1


async def test_a_resent_report_is_a_duplicate(client: httpx.AsyncClient, app: FastAPI) -> None:
    report = crash_report()
    first = await post_crash(client, report)
    again = await post_crash(client, report, compress=True)
    assert first.status_code == 201
    assert again.status_code == 200
    assert again.json() == {"id": first.json()["id"], "duplicate": True}
    assert len(await _rows(app)) == 1


async def test_two_copies_at_once_are_stored_once(client: httpx.AsyncClient, app: FastAPI) -> None:
    report = crash_report()
    responses = await asyncio.gather(*(post_crash(client, report) for _ in range(4)))
    statuses = sorted(r.status_code for r in responses)
    assert statuses == [200, 200, 200, 201]
    assert len({r.json()["id"] for r in responses}) == 1
    assert len(await _rows(app)) == 1
    assert len(await _dumps(app)) == 1


@pytest.mark.parametrize("install", [None, "", "ABCDEF0123456789ABCDEF0123456789", "abc", "g" * 32])
async def test_the_install_id_is_required(client: httpx.AsyncClient, install: str | None) -> None:
    response = await post_crash(client, crash_report(), install=install)
    assert response.status_code == 400
    assert _error(response)["code"] == "invalid_install_id"


def _deep(depth: int) -> Any:
    value: Any = "leaf"
    for _ in range(depth):
        value = {"x": value}
    return value


MARKER = "MARKER-VALUE-1234"


def _with(path: str, value: Any) -> dict[str, Any]:
    report = crash_report()
    *parents, last = path.split(".")
    node = report
    for part in parents:
        node = node[part]
    node[last] = value
    return report


def _without(path: str) -> dict[str, Any]:
    report = crash_report()
    *parents, last = path.split(".")
    node = report
    for part in parents:
        node = node[part]
    del node[last]
    return report


@pytest.mark.parametrize(
    "body",
    [
        pytest.param(b"{not json", id="not-json"),
        pytest.param(b"[1, 2]", id="not-an-object"),
        pytest.param(b"\xff\xfe{}", id="not-utf8"),
        pytest.param(json.dumps(crash_report()).replace('"ram_mib": 65536', '"ram_mib": NaN').encode(), id="nan"),
        pytest.param(_with("schema", "xerces.crash/2"), id="schema"),
        pytest.param(_with("report_id", MARKER), id="report-id"),
        pytest.param(_with("report_id", "A" * 32), id="report-id-upper"),
        pytest.param(_with("created_utc", MARKER), id="created"),
        pytest.param(_with("created_utc", "0001-01-01T00:00:00+05:00"), id="created-range"),
        pytest.param(_without("crash"), id="no-crash"),
        pytest.param(_without("crash.kind"), id="no-kind"),
        pytest.param(_with("crash.kind", MARKER), id="kind"),
        pytest.param(_without("xerces.version"), id="no-version"),
        pytest.param(_with("crash.code", MARKER), id="code"),
        pytest.param(_with("crash.offset", "1a2b"), id="offset-no-0x"),
        pytest.param(_with("launch.mode", "xx"), id="mode"),
        pytest.param(_with("crash.module", "m" * 257), id="long-name"),
        pytest.param(_with("crash.frames", ["f+0x1"] * 33), id="frames"),
        pytest.param(_with("logs.game_log_tail", "x" * (64 * 1024 + 1)), id="long-tail"),
        pytest.param(_with("system.gpus", [{"name": "gpu"}] * 17), id="gpus"),
        pytest.param(_with("system.ram_mib", -1), id="negative"),
        pytest.param(_with("logs.journal", "not an object"), id="journal-type"),
        pytest.param(_with("logs.journal", _deep(40)), id="journal-deep"),
        pytest.param(_with("logs.journal", {"x": "y" * (300 * 1024)}), id="journal-big"),
        pytest.param(_with("minidump_b64", "not base64!"), id="dump-base64"),
        pytest.param(_with("minidump_b64", base64.b64encode(b"ZIP!data").decode()), id="dump-magic"),
    ],
)
async def test_invalid_reports_are_400(client: httpx.AsyncClient, app: FastAPI, body: Any) -> None:
    response = await post_crash(client, body)
    assert response.status_code == 400, response.text
    assert _error(response)["code"] == "invalid_crash_report"
    assert MARKER not in response.text  # the message names the field, never the value
    assert await _rows(app) == []


async def test_very_deep_json_is_400(client: httpx.AsyncClient) -> None:
    body = b'{"logs": {"journal": ' + b"[" * 200_000 + b"]" * 200_000 + b"}}"
    response = await post_crash(client, body)
    assert response.status_code == 400


async def test_unknown_fields_are_ignored(client: httpx.AsyncClient, app: FastAPI) -> None:
    report = crash_report(future_part={"x": 1})
    report["crash"]["new_field"] = "kept out"
    report["system"]["motherboard"] = "kept out"
    response = await post_crash(client, report)
    assert response.status_code == 201, response.text
    [row] = await _rows(app)
    assert "new_field" not in row.crash
    assert "motherboard" not in (row.system or {})


async def test_only_the_required_parts_are_needed(client: httpx.AsyncClient, app: FastAPI) -> None:
    response = await post_crash(client, minimal_report())
    assert response.status_code == 201, response.text
    [row] = await _rows(app)
    assert row.kind == "unknown_exit"
    for column in ("commit", "game_build", "code", "module", "offset", "phase", "mode", "map"):
        assert getattr(row, column) is None, column
    assert (row.system, row.mods, row.logs, row.flags) == (None, None, None, None)
    assert row.signature == crash_signature("unknown_exit", None, None, None)
    assert await _dumps(app) == {}


async def test_naive_created_times_count_as_utc(client: httpx.AsyncClient, app: FastAPI) -> None:
    response = await post_crash(client, minimal_report(created_utc="2026-11-05T13:30:00"))
    assert response.status_code == 201
    [row] = await _rows(app)
    assert row.created_utc.isoformat() == "2026-11-05T13:30:00+00:00"


async def test_bodies_over_4_mib_are_413(client: httpx.AsyncClient, app: FastAPI) -> None:
    raw = await post_crash(client, b"x" * (4 * MIB + 1))
    assert raw.status_code == 413
    assert "4 MiB" in _error(raw)["message"]
    # A small gzip body that inflates past the limit stops at the limit.
    bomb = json.dumps(_with("logs.game_log_tail", "")).encode().replace(
        b'"game_log_tail": ""', b'"game_log_tail": "' + b"0" * (5 * MIB) + b'"'
    )
    assert len(gzip.compress(bomb)) < 64 * 1024
    inflated = await post_crash(client, bomb, compress=True)
    assert inflated.status_code == 413
    assert await _rows(app) == []


async def test_chunked_bodies_cannot_slip_past_the_limit(app_factory: AppFactory) -> None:
    app = await app_factory(crash_max_body_bytes=64 * 1024)

    async def chunks() -> AsyncIterator[bytes]:
        for _ in range(10):
            yield b" " * (10 * 1024)  # no Content-Length: the middleware counts as it reads

    async with client_for(app) as client:
        response = await client.post(
            "/v1/crashes",
            content=chunks(),
            headers={"Content-Type": "application/json", "X-Xerces-Install": INSTALL},
        )
        assert response.status_code == 413
        assert "64 KB" in _error(response)["message"]
        assert (await post_crash(client, minimal_report())).status_code == 201


async def test_the_minidump_is_capped_at_2_mib(client: httpx.AsyncClient, app: FastAPI) -> None:
    largest = minidump(b"\x01" * (2 * MIB - len(minidump())))
    ok = await post_crash(client, crash_report(minidump_b64=base64.b64encode(largest).decode()))
    assert ok.status_code == 201, ok.text
    too_big = await post_crash(
        client, crash_report(minidump_b64=base64.b64encode(largest + b"\x01").decode())
    )
    assert too_big.status_code == 413
    assert "2 MiB" in _error(too_big)["message"]
    assert len(await _rows(app)) == 1


def _padded(room: int, filler: str, *, dump: bytes | None = None) -> bytes:
    """A valid report whose unknown field `junk` (a list of `filler`) brings everything but the
    minidump to about `room` bytes."""
    report = crash_report(minidump_b64=base64.b64encode(dump).decode() if dump else None)
    report["junk"] = "__FILL__"
    text = json.dumps(report)
    rest = len(text) - (len(report["minidump_b64"]) if dump else 0)
    count = (room - rest) // (len(filler) + 1)
    return text.replace('"__FILL__"', "[" + ",".join([filler] * count) + "]").encode()


LARGEST_DUMP = minidump(b"\x01" * (2 * MIB - len(minidump())))


async def test_the_report_without_its_minidump_is_capped_at_1_mib(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    # The minidump does not count, so the largest one fits next to almost 1 MiB of the rest.
    fits = _padded(CRASH_REST_MAX - 64, '"ab"', dump=LARGEST_DUMP)
    assert len(fits) > 3 * MIB
    assert (await post_crash(client, fits)).status_code == 201
    over = await post_crash(client, _padded(CRASH_REST_MAX + 64, '"ab"'))
    assert over.status_code == 413
    assert "without its minidump is larger than 1 MiB" in _error(over)["message"]
    # A second minidump field (here in an unknown part) is no way around it.
    report = crash_report()
    report["junk"] = {"minidump_b64": report["minidump_b64"], "more": "x" * CRASH_REST_MAX}
    assert (await post_crash(client, report)).status_code == 413
    assert len(await _rows(app)) == 1


def test_a_worst_case_report_costs_little_memory_and_time() -> None:
    # Tiny JSON values are the costliest per byte: about 70 bytes of Python objects for each
    # 3-byte "{}". Unknown fields are dropped by validation untouched, and nothing is copied
    # whole, so the largest such body costs some 30 MB and a fraction of a second.
    body = _padded(CRASH_REST_MAX - 64, "{}", dump=LARGEST_DUMP)
    tracemalloc.start()
    started = time.perf_counter()
    try:
        prepared = prepare_body(body)
        _, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    elapsed = time.perf_counter() - started
    assert prepared.minidump is not None
    assert peak < 48 * MIB, peak
    assert elapsed < 2.0, elapsed


async def test_strings_near_their_cap_survive_the_scrub(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    # <user> is longer than "Al": checked as sent, then cut back after the scrub.
    module = "C:\\Users\\Al\\" + "m" * (256 - 12)
    tail = ("C:\\Users\\Al\\x\n" * 6000)[: 64 * 1024] + "the last line"
    tail = tail[-64 * 1024 :]
    report = crash_report()
    report["crash"]["module"] = module
    report["xerces"]["version"] = "C:\\Users\\Al\\" + "v" * (64 - 12)
    report["logs"]["game_log_tail"] = tail
    response = await post_crash(client, report)
    assert response.status_code == 201, response.text
    [row] = await _rows(app)
    assert row.module is not None and len(row.module) == 256
    assert row.module.startswith("C:\\Users\\<user>\\mmm")
    assert len(row.version) == 64 and row.version.startswith("C:\\Users\\<user>\\")
    stored_tail = row.logs["game_log_tail"]
    assert len(stored_tail) == 64 * 1024 and stored_tail.endswith("the last line")
    assert "\\Al\\" not in stored_tail


async def test_short_fields_lose_control_characters(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    esc = "\x1b"
    report = crash_report()
    report["crash"]["module"] = "Black" + esc + "[2JOps3.exe\x9b\t"
    report["crash"]["frames"] = ["f" + esc + "]52;c;Y3VybA==\x07+0x1"]
    report["launch"]["map"] = "zm\r\nzod"
    report["logs"]["game_log_tail"] = "colour " + esc + "[31mred\r\nnext\tline"
    response = await post_crash(client, report)
    assert response.status_code == 201, response.text
    [row] = await _rows(app)
    assert row.module == "Black[2JOps3.exe "
    assert row.crash["frames"] == ["f]52;c;Y3VybA==+0x1"]
    assert row.map == "zm  zod"
    # Logs are kept as sent; scripts/crashes.py escapes them when it prints them.
    assert row.logs["game_log_tail"] == report["logs"]["game_log_tail"]


async def test_other_routes_keep_the_128_kb_limit(client: httpx.AsyncClient) -> None:
    response = await client.post("/v1/packs", content=b"x" * (200 * 1024))
    assert response.status_code == 413
    assert "128 KB" in _error(response)["message"]


@pytest.mark.parametrize(
    ("headers", "code"),
    [
        ({"Content-Type": "text/plain"}, "unsupported_media_type"),
        ({"Content-Type": "application/json; charset=latin-1"}, "unsupported_media_type"),
        ({"Content-Encoding": "br"}, "unsupported_media_type"),
        ({"Content-Encoding": "gzip"}, "invalid_crash_report"),  # the body is not gzip
    ],
)
async def test_media_type_and_encoding(
    client: httpx.AsyncClient, headers: dict[str, str], code: str
) -> None:
    # 400, not 415: launchers drop a report on 400 and would keep retrying a 415 for 7 days.
    response = await post_crash(client, crash_report(), headers=headers)
    assert response.status_code == 400, response.text
    assert _error(response)["code"] == code


async def test_gzip_with_trailing_bytes_is_400(client: httpx.AsyncClient) -> None:
    body = gzip.compress(json.dumps(crash_report()).encode()) + b"junk"
    response = await post_crash(
        client, body, headers={"Content-Encoding": "gzip"}
    )
    assert response.status_code == 400


async def test_utf8_charset_and_identity_are_fine(client: httpx.AsyncClient) -> None:
    response = await post_crash(
        client,
        crash_report(),
        headers={"Content-Type": "application/json; charset=UTF-8", "Content-Encoding": "identity"},
    )
    assert response.status_code == 201, response.text


async def test_reports_are_limited_per_install(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("crash_install"))
    async with client_for(app) as client:
        for _ in range(20):  # a launcher's whole outbox at once
            assert (await post_crash(client, minimal_report())).status_code == 201
        limited = await post_crash(client, minimal_report())
        assert limited.status_code == 429
        assert int(limited.headers["Retry-After"]) >= 1
        other = await post_crash(client, minimal_report(), install=new_hex_id())
        assert other.status_code == 201


async def test_reports_are_limited_per_address(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("crash_address"))
    async with client_for(app) as client:
        address = "198.51.100.20"
        for _ in range(60):  # new install ids do not help
            sent = await post_crash(client, minimal_report(), install=new_hex_id(), address=address)
            assert sent.status_code == 201
        limited = await post_crash(client, minimal_report(), install=new_hex_id(), address=address)
        assert limited.status_code == 429
        elsewhere = await post_crash(client, minimal_report(), install=new_hex_id())
        assert elsewhere.status_code == 201


async def test_all_stored_reports_together_are_limited(app_factory: AppFactory) -> None:
    capped = await app_factory(rules={**RELAXED_RULES, "crash_all": Rule(3, 86400)})
    async with client_for(capped) as client:
        # Rejected and repeated reports store nothing, so they use none of the day's room.
        for _ in range(5):
            assert (await post_crash(client, b"{broken", install=new_hex_id())).status_code == 400
        report = minimal_report()
        for status in (201, 200, 200):
            assert (await post_crash(client, report)).status_code == status
        for _ in range(2):
            response = await post_crash(client, minimal_report(), install=new_hex_id())
            assert response.status_code == 201
        limited = await post_crash(client, minimal_report(), install=new_hex_id())
        assert limited.status_code == 429
        # A resend of a stored report still gets its answer.
        assert (await post_crash(client, report)).status_code == 200


def _stored_size(report: dict[str, Any]) -> int:
    return prepare_body(json.dumps(report).encode()).stored_bytes


async def test_an_address_pays_by_the_byte_too(app_factory: AppFactory) -> None:
    small = minimal_report()
    room = len(json.dumps(small)) * 3
    app = await app_factory(rules={**RELAXED_RULES, "crash_address_bytes": Rule(room, HOUR)})
    async with client_for(app) as client:
        address = "198.51.100.40"
        for _ in range(2):
            sent = await post_crash(client, minimal_report(), address=address)
            assert sent.status_code == 201
        big = minimal_report(logs={"game_log_tail": "x" * (room // 2)})
        limited = await post_crash(client, big, address=address)
        assert limited.status_code == 429
        assert int(limited.headers["Retry-After"]) >= 1
        assert (await post_crash(client, big)).status_code == 201  # another address


async def test_stored_bytes_per_day_are_limited(app_factory: AppFactory) -> None:
    size = _stored_size(crash_report())
    app = await app_factory(rules={**RELAXED_RULES, "crash_bytes": Rule(size * 2.5, 86400)})
    async with client_for(app) as client:
        stored = crash_report()
        assert (await post_crash(client, stored)).status_code == 201
        assert (await post_crash(client, crash_report())).status_code == 201
        limited = await post_crash(client, crash_report())
        assert limited.status_code == 429
        # Resends store nothing and still get their answer.
        assert (await post_crash(client, stored)).status_code == 200


async def test_storage_has_a_ceiling(
    app_factory: AppFactory, tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    caplog.set_level(logging.INFO)
    settings = make_settings(tmp_path, XERCES_CRASH_STORAGE_MB=1)
    app = await app_factory(settings=settings)
    third = base64.b64encode(minidump(b"\x02" * (400 * 1024))).decode()  # of 1 MiB
    async with client_for(app) as client:
        kept = []
        for _ in range(2):
            report = crash_report(minidump_b64=third)
            assert (await post_crash(client, report)).status_code == 201
            kept.append(report)
        full = await post_crash(client, crash_report(minidump_b64=third), install=new_hex_id())
        assert full.status_code == 429
        assert full.headers["Retry-After"] == "3600"
        assert _error(full)["code"] == "crash_storage_full"
        assert any(record.getMessage() == "crash_storage_full" for record in caplog.records)
        assert (await post_crash(client, kept[0])).status_code == 200  # known ones still answer
        # Room comes back when reports go (deleted by their install, or after 90 days).
        await client.delete("/v1/crashes", headers={"X-Xerces-Install": INSTALL})
        assert (await post_crash(client, crash_report(minidump_b64=third))).status_code == 201
    async with services_of(app).sessionmaker() as db:
        total = sum((await db.scalars(select(CrashReport.stored_bytes))).all())
    assert total <= MIB


async def test_rejected_reports_count_too(app_factory: AppFactory) -> None:
    app = await app_factory(rules={**RELAXED_RULES, "crash_install": Rule(2, 3600)})
    async with client_for(app) as client:
        for _ in range(2):
            assert (await post_crash(client, b"{broken")).status_code == 400
        assert (await post_crash(client, minimal_report())).status_code == 429


async def _slow_body(started: asyncio.Event, release: asyncio.Event) -> AsyncIterator[bytes]:
    body = json.dumps(minimal_report()).encode()
    yield body[:10]
    started.set()
    await release.wait()
    yield body[10:]


async def test_big_uploads_take_turns(app_factory: AppFactory) -> None:
    app = await app_factory(crash_upload_slots=1)
    async with client_for(app) as client:
        started, release = asyncio.Event(), asyncio.Event()
        slow = asyncio.create_task(
            client.post(
                "/v1/crashes",
                content=_slow_body(started, release),
                headers={"Content-Type": "application/json", "X-Xerces-Install": INSTALL},
            )
        )
        await asyncio.wait_for(started.wait(), 3)
        busy = await post_crash(client, minimal_report())
        assert busy.status_code == 503
        assert busy.headers["Retry-After"] == "10"
        # Other routes are not held up.
        assert (await client.get("/healthz")).status_code == 200
        release.set()
        assert (await slow).status_code == 201
        assert (await post_crash(client, minimal_report())).status_code == 201


async def test_one_address_holds_at_most_its_share_of_slots(app_factory: AppFactory) -> None:
    app = await app_factory(crash_upload_slots=4, crash_upload_slots_per_address=1)
    async with client_for(app) as client:
        started, release = asyncio.Event(), asyncio.Event()
        here = "198.51.100.50"
        slow = asyncio.create_task(
            client.post(
                "/v1/crashes",
                content=_slow_body(started, release),
                headers={
                    "Content-Type": "application/json",
                    "X-Xerces-Install": INSTALL,
                    "X-Forwarded-For": here,
                },
            )
        )
        await asyncio.wait_for(started.wait(), 3)
        busy = await post_crash(client, minimal_report(), address=here)
        assert busy.status_code == 503 and busy.headers["Retry-After"] == "10"
        assert (await post_crash(client, minimal_report())).status_code == 201  # another address
        release.set()
        assert (await slow).status_code == 201
        assert (await post_crash(client, minimal_report(), address=here)).status_code == 201


async def test_an_address_over_its_limit_takes_no_slot(app_factory: AppFactory) -> None:
    rules = {**RELAXED_RULES, "crash_address": Rule(1, HOUR)}
    app = await app_factory(rules=rules, crash_upload_slots=1)
    async with client_for(app) as client:
        sender = "198.51.100.60"
        assert (await post_crash(client, minimal_report(), address=sender)).status_code == 201
        started, release = asyncio.Event(), asyncio.Event()
        held = await asyncio.wait_for(
            client.post(
                "/v1/crashes",
                content=_slow_body(started, release),
                headers={
                    "Content-Type": "application/json",
                    "X-Xerces-Install": INSTALL,
                    "X-Forwarded-For": sender,
                },
            ),
            3,
        )
        # Refused before its body was read, so it never held the only slot.
        assert held.status_code == 429 and not started.is_set()
        assert (await post_crash(client, minimal_report())).status_code == 201


async def test_uploads_refused_by_the_middleware_count_too(app_factory: AppFactory) -> None:
    rules = {**RELAXED_RULES, "crash_address": Rule(2, HOUR)}
    app = await app_factory(rules=rules, crash_body_timeout=0.2)
    async with client_for(app) as client:
        sender = "198.51.100.70"
        too_big = await post_crash(client, b"x" * (4 * MIB + 1), address=sender)
        assert too_big.status_code == 413
        started, release = asyncio.Event(), asyncio.Event()
        slow = await client.post(
            "/v1/crashes",
            content=_slow_body(started, release),
            headers={
                "Content-Type": "application/json",
                "X-Xerces-Install": INSTALL,
                "X-Forwarded-For": sender,
            },
        )
        release.set()
        assert slow.status_code == 408
        assert (await post_crash(client, minimal_report(), address=sender)).status_code == 429


async def test_deletes_take_no_upload_slot(app_factory: AppFactory) -> None:
    app = await app_factory(crash_upload_slots=1)
    async with client_for(app) as client:
        started, release = asyncio.Event(), asyncio.Event()
        slow = asyncio.create_task(
            client.post(
                "/v1/crashes",
                content=_slow_body(started, release),
                headers={"Content-Type": "application/json", "X-Xerces-Install": INSTALL},
            )
        )
        await asyncio.wait_for(started.wait(), 3)
        deleted = await client.delete("/v1/crashes", headers={"X-Xerces-Install": new_hex_id()})
        assert deleted.status_code == 200
        big = await client.request(
            "DELETE",
            "/v1/crashes",
            content=b"x" * (200 * 1024),
            headers={"X-Xerces-Install": new_hex_id()},
        )
        assert big.status_code == 413 and "128 KB" in _error(big)["message"]
        release.set()
        assert (await slow).status_code == 201


async def test_a_slow_upload_times_out(app_factory: AppFactory) -> None:
    app = await app_factory(crash_body_timeout=0.2)
    async with client_for(app) as client:
        started, release = asyncio.Event(), asyncio.Event()
        response = await client.post(
            "/v1/crashes",
            content=_slow_body(started, release),
            headers={"Content-Type": "application/json", "X-Xerces-Install": INSTALL},
        )
        release.set()
        assert response.status_code == 408
        assert (await post_crash(client, minimal_report())).status_code == 201  # slot given back


# ----------------------------------------------------------------------------- deletion


async def test_delete_removes_every_report_of_the_install(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    mine, theirs = new_hex_id(), new_hex_id()
    assert (await post_crash(client, crash_report(), install=mine)).status_code == 201
    assert (await post_crash(client, minimal_report(), install=mine)).status_code == 201
    kept = await post_crash(client, crash_report(), install=theirs)
    assert kept.status_code == 201

    response = await client.delete("/v1/crashes", headers={"X-Xerces-Install": mine})
    assert response.status_code == 200
    assert response.json() == {"deleted": 2}
    rows = await _rows(app)
    assert [str(row.id) for row in rows] == [kept.json()["id"]]
    assert set(map(str, await _dumps(app))) == {kept.json()["id"]}  # the minidump went too

    again = await client.delete("/v1/crashes", headers={"X-Xerces-Install": mine})
    assert again.json() == {"deleted": 0}
    # A deleted report can be sent again: it is new to the server.
    assert (await post_crash(client, minimal_report(), install=mine)).status_code == 201


async def test_delete_needs_an_install_id(client: httpx.AsyncClient) -> None:
    for headers in ({}, {"X-Xerces-Install": "nope"}):
        response = await client.delete("/v1/crashes", headers=headers)
        assert response.status_code == 400
        assert _error(response)["code"] == "invalid_install_id"


async def test_deletes_are_rate_limited(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("crash_delete"))
    async with client_for(app) as client:
        headers = {"X-Xerces-Install": INSTALL, "X-Forwarded-For": "198.51.100.30"}
        for _ in range(10):
            assert (await client.delete("/v1/crashes", headers=headers)).status_code == 200
        assert (await client.delete("/v1/crashes", headers=headers)).status_code == 429


# ----------------------------------------------------------------------------- signature


def test_the_signature_is_the_documented_hash() -> None:
    frames = ["BlackOps3.exe+0x001A2B", "boiii.exe+0x20", "ntdll.dll+0x30", "kernel32.dll+0x40"]
    text = "exception|blackops3.exe|0x1a2b|blackops3.exe+0x1a2b|boiii.exe+0x20|ntdll.dll+0x30"
    expected = hashlib.sha256(text.encode()).hexdigest()[:16]
    assert crash_signature("exception", "BlackOps3.exe", "0x00001A2B", frames) == expected
    # Case, leading zeros and frames past the third do not split a group.
    assert crash_signature("exception", "blackops3.EXE", "0x1a2b", frames[:3]) == expected
    assert crash_signature("exception", "BlackOps3.exe", "0x1a2c", frames) != expected
    assert crash_signature("fail_fast", "BlackOps3.exe", "0x1a2b", frames) != expected
    empty = hashlib.sha256(b"unknown_exit||").hexdigest()[:16]
    assert crash_signature("unknown_exit", None, None, None) == empty


def test_crash_rules_match_the_readme() -> None:
    def per(name: str) -> tuple[float, float]:
        return DEFAULT_RULES[name].capacity, DEFAULT_RULES[name].period

    assert per("crash_install") == (20, 3600)
    assert per("crash_address") == (60, 3600)
    assert per("crash_address_bytes") == (64 * MIB, 3600)
    assert per("crash_all") == (2000, 86400)
    assert per("crash_bytes") == (256 * MIB, 86400)
    assert per("crash_delete") == (10, 3600)
    assert per("admin_auth") == (10, 900)
