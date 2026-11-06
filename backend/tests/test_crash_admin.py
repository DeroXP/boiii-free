"""Admin routes for crash reports: the token, listing, one report, minidumps and groups."""

import base64
import uuid
from datetime import timedelta
from pathlib import Path

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import update

from app.models import CrashReport, utcnow
from tests.crashdata import (
    ADMIN_TOKEN,
    admin_headers,
    crash_report,
    minidump,
    minimal_report,
    new_hex_id,
    post_crash,
)
from tests.helpers import AppFactory, client_for, make_settings, services_of, strict_rules

ADMIN_PATHS = (
    "/v1/admin/crashes",
    "/v1/admin/crashes?limit=9999&since=yesterday",
    f"/v1/admin/crashes/{uuid.uuid4()}",
    "/v1/admin/crashes/not-a-uuid",
    f"/v1/admin/crashes/{uuid.uuid4()}/minidump",
    "/v1/admin/crash-groups",
    "/v1/admin/crash-groups?limit=0",
)


@pytest.fixture
async def admin_app(app_factory: AppFactory, tmp_path: Path) -> FastAPI:
    return await app_factory(settings=make_settings(tmp_path, XERCES_ADMIN_TOKEN=ADMIN_TOKEN))


@pytest.fixture
async def admin_client(admin_app: FastAPI):
    async with client_for(admin_app) as client:
        yield client


async def _set_received(app: FastAPI, crash_id: str, when) -> None:
    async with services_of(app).sessionmaker() as db:
        await db.execute(
            update(CrashReport).where(CrashReport.id == uuid.UUID(crash_id)).values(received_at=when)
        )
        await db.commit()


async def _send(client: httpx.AsyncClient, report=None, **kwargs) -> str:
    response = await post_crash(client, report or crash_report(), **kwargs)
    assert response.status_code == 201, response.text
    return response.json()["id"]


async def test_admin_routes_are_404_when_no_token_is_set(client: httpx.AsyncClient) -> None:
    unknown = await client.get("/v1/no-such-route")
    for path in ADMIN_PATHS:
        for headers in ({}, admin_headers(), {"Authorization": "Bearer "}):
            response = await client.get(path, headers=headers)
            assert response.status_code == 404, (path, response.text)
            assert response.json() == unknown.json()


async def test_the_admin_token_is_checked(admin_client: httpx.AsyncClient) -> None:
    for headers in (
        {},
        {"Authorization": "Bearer"},
        {"Authorization": "Bearer wrong-token"},
        {"Authorization": f"Bearer {ADMIN_TOKEN}x"},
        {"Authorization": f"Basic {ADMIN_TOKEN}"},
        {"Authorization": ADMIN_TOKEN},
    ):
        response = await admin_client.get("/v1/admin/crashes", headers=headers)
        assert response.status_code == 401, headers
        assert response.headers["WWW-Authenticate"] == "Bearer"
    assert (await admin_client.get("/v1/admin/crashes", headers=admin_headers())).status_code == 200
    lower = {"Authorization": f"bearer  {ADMIN_TOKEN} "}
    assert (await admin_client.get("/v1/admin/crashes", headers=lower)).status_code == 200


async def test_wrong_admin_tokens_are_rate_limited(app_factory: AppFactory, tmp_path: Path) -> None:
    settings = make_settings(tmp_path, XERCES_ADMIN_TOKEN=ADMIN_TOKEN)
    app = await app_factory(settings=settings, rules=strict_rules("admin_auth"))
    address = "198.51.100.40"
    async with client_for(app) as client:
        for _ in range(30):  # right tokens never count
            ok = await client.get("/v1/admin/crashes", headers=admin_headers(address=address))
            assert ok.status_code == 200
        wrong = admin_headers("wrong", address=address)
        for _ in range(10):
            assert (await client.get("/v1/admin/crashes", headers=wrong)).status_code == 401
        limited = await client.get("/v1/admin/crashes", headers=wrong)
        assert limited.status_code == 429
        # Even the right token waits now from that address, but not from another.
        right = await client.get("/v1/admin/crashes", headers=admin_headers(address=address))
        assert right.status_code == 429
        assert (await client.get("/v1/admin/crashes", headers=admin_headers())).status_code == 200


async def test_list_is_newest_first_with_filters_and_pages(
    admin_client: httpx.AsyncClient, admin_app: FastAPI
) -> None:
    now = utcnow()
    ids = []
    for index in range(7):
        version = "0.1.0-beta.2" if index % 2 else "0.1.0-beta.1"
        ids.append(await _send(admin_client, crash_report(xerces={"version": version})))
    odd = await _send(admin_client, minimal_report())
    # Three of them share one second, so the pages must break ties by id.
    times = [now - timedelta(hours=10 - index) for index in range(7)]
    times[2] = times[3] = times[4]
    for crash_id, when in zip(ids, times, strict=True):
        await _set_received(admin_app, crash_id, when)
    await _set_received(admin_app, odd, now - timedelta(days=30))

    seen = []
    cursor = None
    while True:
        params = {"limit": 2} | ({"cursor": cursor} if cursor else {})
        page = (await admin_client.get("/v1/admin/crashes", params=params, headers=admin_headers())).json()
        assert len(page["items"]) <= 2
        seen.extend(page["items"])
        cursor = page["next_cursor"]
        if cursor is None:
            break
    assert len(seen) == 8 and len({item["id"] for item in seen}) == 8
    keys = [(item["received_at"], item["id"]) for item in seen]
    assert keys == sorted(keys, reverse=True)
    assert seen[-1]["id"] == odd
    first = seen[0]
    assert set(first) == {
        "id", "report_id", "install_id", "received_at", "created_utc", "version", "commit",
        "game_build", "kind", "code", "module", "offset", "signature", "phase", "mode", "map",
        "has_minidump", "minidump_size",
    }  # metadata only: no logs, system or minidump
    assert first["has_minidump"] is True and first["minidump_size"] == len(minidump(b"stack"))

    async def listed(**params) -> list[str]:
        response = await admin_client.get("/v1/admin/crashes", params=params, headers=admin_headers())
        assert response.status_code == 200, response.text
        return [item["id"] for item in response.json()["items"]]

    since = (now - timedelta(days=1)).isoformat().replace("+00:00", "Z")
    assert set(await listed(since=since)) == set(ids)
    assert set(await listed(version="0.1.0-beta.2")) == {ids[1], ids[3], ids[5]}
    signature = seen[-1]["signature"]
    assert await listed(signature=signature) == [odd]
    assert await listed(signature="0" * 16) == []

    bad = await admin_client.get("/v1/admin/crashes", params={"cursor": "nonsense"}, headers=admin_headers())
    assert bad.status_code == 400 and bad.json()["error"]["code"] == "invalid_cursor"
    for params in ({"limit": 201}, {"limit": 0}, {"signature": "XYZ"}):
        response = await admin_client.get("/v1/admin/crashes", params=params, headers=admin_headers())
        assert response.status_code == 422, params


async def test_one_report_in_full_without_the_minidump_bytes(
    admin_client: httpx.AsyncClient,
) -> None:
    report = crash_report()
    crash_id = await _send(admin_client, report, install="a" * 32)
    response = await admin_client.get(f"/v1/admin/crashes/{crash_id}", headers=admin_headers())
    assert response.status_code == 200
    body = response.json()
    assert body["id"] == crash_id
    assert body["install_id"] == "a" * 32
    assert body["has_minidump"] is True
    assert body["minidump_size"] == len(minidump(b"stack"))
    assert "minidump_b64" not in body and "data" not in body
    for part in ("schema", "report_id", "xerces", "game", "system", "launch", "crash", "logs"):
        assert body[part] == report[part], part
    assert body["created_utc"] == "2026-11-05T12:00:00Z"

    bare = await _send(admin_client, minimal_report())
    body = (await admin_client.get(f"/v1/admin/crashes/{bare}", headers=admin_headers())).json()
    assert body["has_minidump"] is False and body["minidump_size"] is None
    assert body["system"] is None and body["logs"] is None
    assert body["game"] == {"build": None, "steam_build": None}

    missing = await admin_client.get(f"/v1/admin/crashes/{uuid.uuid4()}", headers=admin_headers())
    assert missing.status_code == 404


async def test_minidump_download(admin_client: httpx.AsyncClient) -> None:
    dump = minidump(b"\x00" * 100, b"stacks only")
    crash_id = await _send(
        admin_client, crash_report(minidump_b64=base64.b64encode(dump).decode())
    )
    response = await admin_client.get(
        f"/v1/admin/crashes/{crash_id}/minidump", headers=admin_headers()
    )
    assert response.status_code == 200
    assert response.content == dump
    assert response.headers["content-type"] == "application/octet-stream"
    assert response.headers["content-disposition"] == f'attachment; filename="{crash_id}.dmp"'
    assert response.headers["cache-control"] == "no-store"

    bare = await _send(admin_client, minimal_report())
    for crash_id in (bare, str(uuid.uuid4())):
        none = await admin_client.get(
            f"/v1/admin/crashes/{crash_id}/minidump", headers=admin_headers()
        )
        assert none.status_code == 404


async def test_crash_groups(admin_client: httpx.AsyncClient, admin_app: FastAPI) -> None:
    now = utcnow()

    def report(version: str, map_name: str | None, mods: list[dict[str, str]]):
        base = crash_report()
        base["xerces"]["version"] = version
        base["launch"]["map"] = map_name
        base["launch"]["mods"] = mods
        return base

    weapons = {"id": "ws.1", "name": "Weapons", "version": "1"}
    zombies = {"id": "ws.2", "name": "Zombies+", "version": "2"}
    first = await _send(admin_client, report("0.1.0-beta.1", "zm_castle", [weapons, zombies]), install="1" * 32)
    second = await _send(admin_client, report("0.1.0-beta.1", "zm_castle", [weapons, weapons]), install="1" * 32)
    newest = await _send(admin_client, report("0.1.0-beta.2", "zm_island", [zombies]), install="2" * 32)
    other = await _send(admin_client, minimal_report(), install="3" * 32)
    old = await _send(admin_client, minimal_report(crash={"kind": "fail_fast"}), install="3" * 32)
    for crash_id, age in ((first, 5), (second, 4), (newest, 1), (other, 2), (old, 40 * 24)):
        await _set_received(admin_app, crash_id, now - timedelta(hours=age))

    response = await admin_client.get("/v1/admin/crash-groups", headers=admin_headers())
    assert response.status_code == 200, response.text
    data = response.json()
    assert data["reports"] == 5
    top = data["groups"][0]
    assert [g["count"] for g in data["groups"]] == [3, 1, 1]
    assert data["groups"][1]["sample_id"] == other  # equal counts: the latest first
    assert top["installs"] == 2
    assert top["sample_id"] == newest
    assert top["kind"] == "exception"
    assert (top["module"], top["offset"]) == ("BlackOps3.exe", "0x1a2b3c")
    assert top["frames"] == crash_report()["crash"]["frames"][:3]
    assert top["versions"] == [
        {"value": "0.1.0-beta.1", "count": 2},
        {"value": "0.1.0-beta.2", "count": 1},
    ]
    # A mod listed twice in one report counts once for it.
    assert top["top_mods"] == [
        {"id": "ws.1", "name": "Weapons", "count": 2},
        {"id": "ws.2", "name": "Zombies+", "count": 2},
    ]
    assert top["top_maps"] == [{"value": "zm_castle", "count": 2}, {"value": "zm_island", "count": 1}]
    assert top["first_seen"] < top["last_seen"]

    since = (now - timedelta(days=1)).isoformat().replace("+00:00", "Z")
    recent = await admin_client.get(
        "/v1/admin/crash-groups", params={"since": since, "limit": 1}, headers=admin_headers()
    )
    data = recent.json()
    assert data["reports"] == 4
    assert [g["count"] for g in data["groups"]] == [3]


async def test_reports_from_one_install_list_together(admin_client: httpx.AsyncClient) -> None:
    install = new_hex_id()
    mine = {await _send(admin_client, minimal_report(), install=install) for _ in range(3)}
    await _send(admin_client, minimal_report())
    items = (await admin_client.get("/v1/admin/crashes", headers=admin_headers())).json()["items"]
    assert {item["id"] for item in items if item["install_id"] == install} == mine
