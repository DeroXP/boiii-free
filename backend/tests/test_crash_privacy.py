"""Crash reports keep no client address and no profile path, and an install can delete its own."""

import base64
import json
import logging
import time
from pathlib import Path

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import select

from app.crashes import mask_minidump, scrub, scrub_text
from app.log import JsonFormatter
from app.models import Base, CrashMinidump, CrashReport
from tests.crashdata import INSTALL, crash_report, minidump, post_crash, utf16
from tests.helpers import services_of


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        (r"C:\Users\Bob\AppData\Local\x.dll", r"C:\Users\<user>\AppData\Local\x.dll"),
        ("c:/users/bob/Documents/mod", "c:/users/<user>/Documents/mod"),
        (r"D:\Users\Ann Lee\Saved Games", r"D:\Users\<user>\Saved Games"),
        (r'{"path": "C:\\Users\\Bob\\x"}', r'{"path": "C:\\Users\\<user>\\x"}'),
        (r"\\?\C:\Users\Bob\x", r"\\?\C:\Users\<user>\x"),
        ("file:///C:/Users/Bob/x.txt", "file:///C:/Users/<user>/x.txt"),
        (r"C:\Documents and Settings\Bob\x", r"C:\Documents and Settings\<user>\x"),
        (r"at C:\Users\Bob", r"at C:\Users\<user>"),
        (r"C:\Users\Bob" + "\n" + r"E:\Users\Eve\y", r"C:\Users\<user>" + "\n" + r"E:\Users\<user>\y"),
        ("/home/deck/.steam/steam", "/home/<user>/.steam/steam"),
        (r"Z:\home\deck\Games", r"Z:\home\<user>\Games"),
        # Already redacted, or nobody's: unchanged.
        (r"C:\Users\<user>\x", r"C:\Users\<user>\x"),
        (r"C:\Users\Public\Documents", r"C:\Users\Public\Documents"),
        (r"C:\Users\Default\NTUSER.DAT", r"C:\Users\Default\NTUSER.DAT"),
        ("%USERPROFILE%\\x", "%USERPROFILE%\\x"),
        ("ui/home/menu.lua", "ui/home/menu.lua"),
        ("https://example.com/home/page", "https://example.com/home/page"),
        (r"C:\Program Files (x86)\Steam\steamapps", r"C:\Program Files (x86)\Steam\steamapps"),
        # Text the database could not hold.
        ("a\x00b", "ab"),
        ("lone \ud800 surrogate", "lone ? surrogate"),
    ],
)
def test_scrub_text(text: str, expected: str) -> None:
    assert scrub_text(text) == expected


def test_scrub_walks_keys_and_values() -> None:
    value = {r"C:\Users\Bob\a": [r"C:\Users\Bob\b", {"k": "/home/bob/c"}, 3, None, True]}
    assert scrub(value) == {r"C:\Users\<user>\a": [r"C:\Users\<user>\b", {"k": "/home/<user>/c"}, 3, None, True]}


def test_minidump_names_are_masked_in_place() -> None:
    dump = minidump(
        utf16(r"C:\Users\Bob Smith\AppData\Local\Discord\overlay.dll") + b"\x00\x00",
        b"RSDS" + b"\x11" * 20 + rb"C:\Users\Bob Smith\src\tool.pdb" + b"\x00",
        utf16(r"C:\Users\Public\shared.dll") + b"\x00\x00",
        utf16(r"c:/users/bob/x.dll") + b"\x00\x00",
    )
    masked = mask_minidump(dump)
    assert len(masked) == len(dump)
    assert masked.startswith(b"MDMP")
    for leaked in (utf16("Bob"), utf16("bob"), b"Bob Smith"):
        assert leaked not in masked
    assert utf16(r"C:\Users\_________\AppData\Local\Discord\overlay.dll") in masked
    assert rb"C:\Users\_________\src\tool.pdb" in masked
    assert utf16(r"C:\Users\Public\shared.dll") in masked
    assert utf16(r"c:/users/___/x.dll") in masked


async def test_stored_reports_hold_no_profile_paths(client: httpx.AsyncClient, app: FastAPI) -> None:
    report = crash_report()
    report["crash"]["module"] = r"C:\Users\Bob\AppData\Local\Xerces\xerces_core.dll"
    report["crash"]["frames"] = [r"C:\Users\Bob\mods\bad.dll+0x10"]
    report["launch"]["mods"][0]["name"] = r"C:\Users\Bob\Desktop\mymod"
    report["launch"]["flags"] = [r"-path=C:\Users\Bob\x"]
    report["system"]["cpu"] = "Bob's CPU at /home/bob"
    report["logs"]["game_log_tail"] = "opened C:\\Users\\Bob\\Documents\\x.cfg\nok"
    report["logs"]["journal"] = {r"C:\Users\Bob\key": {"deep": [r"C:/Users/Bob/v"]}}
    report["minidump_b64"] = base64.b64encode(
        minidump(utf16(r"C:\Users\Bob\AppData\x.dll") + b"\x00\x00")
    ).decode()
    response = await post_crash(client, report)
    assert response.status_code == 201, response.text

    async with services_of(app).sessionmaker() as db:
        row = await db.scalar(select(CrashReport))
        dump = await db.scalar(select(CrashMinidump.data))
    assert row is not None and dump is not None
    stored = json.dumps(
        [row.module, row.crash, row.mods, row.flags, row.system, row.logs], ensure_ascii=False
    )
    assert "Bob" not in stored.replace("Bob's", "")
    assert r"C:\\Users\\<user>\\AppData" in stored
    assert row.module == r"C:\Users\<user>\AppData\Local\Xerces\xerces_core.dll"
    assert row.logs["journal"] == {r"C:\Users\<user>\key": {"deep": ["C:/Users/<user>/v"]}}
    assert utf16("Bob") not in dump
    # Grouping sees the scrubbed module, so one crash from many users is one group.
    other = crash_report(crash={**report["crash"], "module": report["crash"]["module"].replace("Bob", "Eve")})
    second = await post_crash(client, other)
    async with services_of(app).sessionmaker() as db:
        signatures = set((await db.scalars(select(CrashReport.signature))).all())
    assert second.status_code == 201 and len(signatures) == 1


def test_no_table_has_an_address_column() -> None:
    suspicious = ("ip", "address", "addr", "forwarded", "user_agent", "agent", "host")
    for table in Base.metadata.tables.values():
        for column in table.columns:
            name = column.name.lower()
            assert not any(part in name.split("_") or name == part for part in suspicious), (
                table.name,
                column.name,
            )
    crash_columns = set(Base.metadata.tables["crash_reports"].columns.keys())
    assert not {"ip", "ip_address", "address", "user_agent"} & crash_columns


async def test_no_address_reaches_the_database_or_the_logs(
    client: httpx.AsyncClient, app: FastAPI, tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    caplog.set_level(logging.DEBUG)
    address = "203.0.113.77"
    agent = "Xerces/9.8.7-unique-agent"
    report = crash_report()
    sent = await post_crash(client, report, address=address, headers={"User-Agent": agent})
    assert sent.status_code == 201
    again = await post_crash(client, report, address=address, compress=True)
    assert again.status_code == 200
    deleted = await client.delete(
        "/v1/crashes", headers={"X-Xerces-Install": "f" * 32, "X-Forwarded-For": address}
    )
    assert deleted.status_code == 200

    await services_of(app).engine.dispose()  # everything written out to the file
    database = (tmp_path / "test.db").read_bytes()
    for secret in (address.encode(), agent.encode(), b"203.0.113"):
        assert secret not in database

    formatter = JsonFormatter()
    text = "\n".join(
        formatter.format(record)
        for record in caplog.records
        if record.name.startswith("xerces_backend") or record.levelno >= logging.WARNING
    )
    for secret in (address, agent, INSTALL, report["report_id"]):
        assert secret not in text
    events = [json.loads(line)["event"] for line in text.splitlines()]
    assert {"crash_received", "crash_duplicate", "crashes_deleted", "request"} <= set(events)


@pytest.mark.parametrize(
    "text",
    [
        "/" * 256 * 1024,
        "\\" * 256 * 1024,
        "C:" + "/" * 256 * 1024,
        r"C:\Users" + "\\" * 256 * 1024,
        ("C:" + "/" * 200) * 1300,
        "/home" * 50_000,
    ],
    ids=["slashes", "backslashes", "drive", "users", "runs", "homes"],
)
def test_scrubbing_takes_linear_time(text: str) -> None:
    # A regular expression that backtracks over a run of slashes from every position would take
    # minutes here (and hold a worker thread and an upload slot all that time).
    started = time.perf_counter()
    scrub_text(text)
    assert time.perf_counter() - started < 1.0


def test_masking_takes_linear_time() -> None:
    dump = minidump((utf16("C:") + utf16("\\") * 1000) * 1000, (b"C:" + b"\\" * 1000) * 1000)
    started = time.perf_counter()
    mask_minidump(dump)
    assert time.perf_counter() - started < 1.0
