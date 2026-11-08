"""Pack share codes."""

import re
import time
from datetime import datetime, timedelta
from typing import Any

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import update

from app.models import Pack, utcnow
from app.routers.packs import looks_like_local_path, new_pack_code
from app.schemas import normalize_pack_code
from tests.helpers import AppFactory, client_for, register, services_of, strict_rules

CODE = re.compile(r"XRC-[0-9A-HJKMNP-TV-Z]{8}")


def pack(**overrides: Any) -> dict[str, Any]:
    body: dict[str, Any] = {
        "schema": 1,
        "id": "7b1e2c4a-1f0d-4f5e-9a51-3c2f8d0e9b11",
        "name": "Weekend Zombies",
        "mode": "zm",
        "map": {"id": "ws.3735179543", "content_hash": "sha256:abc"},
        "mods": [
            {
                "id": "ws.1944930083",
                "version": "0.0.0+ws.1700000000",
                "source": {"workshop": 1944930083},
                "content_hash": "sha256:def",
                "settings": {},
            }
        ],
        "order": ["ws.1944930083"],
        "requires": {"xerces": ">=1.0", "game_build": "0x6531394"},
    }
    body.update(overrides)
    return body


async def test_create_and_fetch(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    created = await client.post("/v1/packs", json={"pack": pack()}, headers=alice.headers)
    assert created.status_code == 201
    body = created.json()
    assert CODE.fullmatch(body["code"])
    expires = datetime.fromisoformat(body["expires_at"])
    assert timedelta(days=89, hours=23) < expires - utcnow() <= timedelta(days=90)

    # No auth needed to fetch; codes are forgiving about case and look-alike letters.
    for code in (body["code"], body["code"].lower(), body["code"].replace("0", "O").replace("1", "I")):
        fetched = await client.get(f"/v1/packs/{code}")
        assert fetched.status_code == 200, code
        assert fetched.json()["code"] == body["code"]
        assert fetched.json()["pack"] == pack()


async def test_unknown_and_malformed_codes(client: httpx.AsyncClient) -> None:
    assert (await client.get("/v1/packs/XRC-00000000")).status_code == 404
    assert (await client.get("/v1/packs/hello")).status_code == 404
    assert (await client.get("/v1/packs/XRC1.abcdef")).status_code == 404


async def test_create_requires_auth(client: httpx.AsyncClient) -> None:
    assert (await client.post("/v1/packs", json={"pack": pack()})).status_code == 401


async def test_size_cap(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    big = pack(notes="n" * (65 * 1024))
    response = await client.post("/v1/packs", json={"pack": big}, headers=alice.headers)
    assert response.status_code == 422
    assert response.json()["error"]["code"] == "invalid_pack"
    assert "64 KB" in response.json()["error"]["message"]


@pytest.mark.parametrize(
    "path",
    [
        "C:\\Users\\alice\\mods\\thing",
        "D:/Games/BO3/mods",
        "see c:\\temp\\x",
        "\\\\server\\share\\mods",
        "\\Windows\\Temp",
        "/home/alice/.local/share",
        "/Users/alice/Library",
        "file:///home/alice/x",
        "~/mods",
        "%USERPROFILE%\\mods",
        # Homes and drives that are not at the start of the path, in any letter case.
        "/var/home/alice/.local/share/Steam",  # Fedora Atomic, Bazzite
        "/mnt/c/Users/alice/Downloads",  # WSL
        "/c/Users/alice/Documents",  # Git Bash, MSYS2
        "/cygdrive/c/Users/alice/mods",
        "/run/media/alice/GAMES/BO3",  # removable drives
        "/media/alice/GAMES/BO3",
        "/users/alice/Library",  # macOS paths ignore case
        "/root/.steam/steam",
        "see /HOME/alice/mods",
        # Network shares and file: URLs anywhere in a string.
        "copied from \\\\NAS\\alice\\bo3-mods",
        "built in \\\\wsl.localhost\\Ubuntu\\home\\alice",
        "smb://nas/alice/mods",
        "see file:///var/home/alice/mods/readme.txt",
        "see ~/mods",
    ],
)
async def test_local_paths_are_rejected(client: httpx.AsyncClient, path: str) -> None:
    alice = await register(client, "alice")
    for candidate in (pack(name=path[:64]), pack(notes={"deep": [path]}), pack(**{path: 1})):
        response = await client.post("/v1/packs", json={"pack": candidate}, headers=alice.headers)
        assert response.status_code == 422, (path, candidate)
        assert "local file path" in response.json()["error"]["message"]


def test_path_detector_allows_normal_values() -> None:
    for value in (
        "https://example.com/home/page",
        "steam://url/CommunityFilePage/123",
        "sha256:abc",
        "ws.1944930083",
        ">=1.0",
        "0x6531394",
        "Weekend Zombies: 2:30 run",
        "https://moddb.com/members/home/",
        "Home/Away",
        "home",
        "Users",
        "root beer",
        "at home/work",
        "Zombies / Home / Users",
        "AC/DC",
        "1/2 users",
        "/r/zombies",
        "example.com/home/page",
        "mods/home/x",
        "Config file: zm.cfg",
        "profile: default",
        "50% off",
    ):
        assert not looks_like_local_path(value), value


def test_path_detector_takes_linear_time() -> None:
    # Long runs that a regex with nested repeats would scan again from every position.
    for value in ("a" * 65_536, "a." * 32_768, "/!" * 32_768, "\\\\" * 32_768, "x://" * 16_384):
        started = time.perf_counter()
        looks_like_local_path(value)
        assert time.perf_counter() - started < 0.5, value[:8]


@pytest.mark.parametrize(
    ("overrides", "message"),
    [
        ({"schema": 2}, "pack.schema must be 1"),
        ({"schema": "1"}, "pack.schema must be 1"),
        ({"schema": True}, "pack.schema must be 1"),
        ({"name": ""}, "pack.name"),
        ({"name": "n" * 81}, "pack.name"),
        ({"name": 5}, "pack.name"),
        ({"mods": "ws.1"}, "pack.mods"),
        ({"mods": [{"id": "ws.1"}] * 129}, "pack.mods"),
        ({"mods": [{"id": "Has-Caps"}]}, "pack.mods[0].id"),
        ({"mods": [{"id": "-leading"}]}, "pack.mods[0].id"),
        ({"mods": [{"id": "x"}]}, "pack.mods[0].id"),
        ({"mods": [{"version": "1"}]}, "pack.mods[0].id"),
        ({"mods": ["ws.1"]}, "pack.mods[0].id"),
    ],
)
async def test_invalid_packs(
    client: httpx.AsyncClient, overrides: dict[str, Any], message: str
) -> None:
    alice = await register(client, "alice")
    response = await client.post("/v1/packs", json={"pack": pack(**overrides)}, headers=alice.headers)
    assert response.status_code == 422, overrides
    assert message in response.json()["error"]["message"]


async def test_nan_is_rejected(client: httpx.AsyncClient) -> None:
    # Python's json module accepts NaN; PostgreSQL's json type does not, so it never gets stored.
    alice = await register(client, "alice")
    raw = b'{"pack": {"schema": 1, "name": "n", "mods": [], "notes": NaN}}'
    response = await client.post(
        "/v1/packs",
        content=raw,
        headers={**alice.headers, "Content-Type": "application/json"},
    )
    assert response.status_code == 422
    assert "NaN" in response.json()["error"]["message"]


@pytest.mark.parametrize(
    "raw",
    [
        b'{"pack": {"schema": 1, "name": "\\ud800", "mods": []}}',
        b'{"pack": {"schema": 1, "name": "n", "mods": [], "notes": {"x": ["ok", "\\udfff"]}}}',
    ],
    ids=["name", "nested"],
)
async def test_lone_surrogate_is_rejected(client: httpx.AsyncClient, raw: bytes) -> None:
    # JSON parsing lets "\ud800" through; it is not valid Unicode and used to end in a 500.
    alice = await register(client, "alice")
    response = await client.post(
        "/v1/packs", content=raw, headers={**alice.headers, "Content-Type": "application/json"}
    )
    assert response.status_code == 422
    assert response.json()["error"] == {
        "code": "invalid_pack",
        "message": "pack contains text that is not valid Unicode.",
    }


async def test_rejected_packs_count_against_the_daily_limit(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("pack_create"))
    async with client_for(app) as client:
        alice = await register(client, "alice")
        for _ in range(20):
            bad = await client.post("/v1/packs", json={"pack": pack(schema=2)}, headers=alice.headers)
            assert bad.status_code == 422
        limited = await client.post("/v1/packs", json={"pack": pack()}, headers=alice.headers)
        assert limited.status_code == 429


async def test_missing_fields_and_wrong_types(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    no_schema = pack()
    del no_schema["schema"]
    no_mods = pack()
    del no_mods["mods"]
    deep: Any = "leaf"
    for _ in range(40):
        deep = [deep]
    for body in (no_schema, no_mods, pack(extra=deep)):
        response = await client.post("/v1/packs", json={"pack": body}, headers=alice.headers)
        assert response.status_code == 422
    not_object = await client.post("/v1/packs", json={"pack": [1, 2]}, headers=alice.headers)
    assert not_object.status_code == 422


async def test_owner_only_delete(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    code = (await client.post("/v1/packs", json={"pack": pack()}, headers=alice.headers)).json()["code"]
    assert (await client.delete(f"/v1/packs/{code}")).status_code == 401
    forbidden = await client.delete(f"/v1/packs/{code}", headers=bob.headers)
    assert forbidden.status_code == 403
    deleted = await client.delete(f"/v1/packs/{code}", headers=alice.headers)
    assert deleted.status_code == 204
    assert (await client.get(f"/v1/packs/{code}")).status_code == 404
    assert (await client.delete(f"/v1/packs/{code}", headers=alice.headers)).status_code == 404


async def test_expired_pack_is_gone(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    code = (await client.post("/v1/packs", json={"pack": pack()}, headers=alice.headers)).json()["code"]
    async with services_of(app).sessionmaker() as db:
        await db.execute(update(Pack).values(expires_at=utcnow() - timedelta(seconds=1)))
        await db.commit()
    assert (await client.get(f"/v1/packs/{code}")).status_code == 404


def test_code_format() -> None:
    codes = {new_pack_code() for _ in range(500)}
    assert len(codes) == 500
    assert all(CODE.fullmatch(code) for code in codes)
    assert normalize_pack_code(" xrc-ab12-cd34 ") == "XRC-AB12CD34"
    assert normalize_pack_code("XRCAB12CD34") == "XRC-AB12CD34"
    assert normalize_pack_code("XRC-OIL00000") == "XRC-01100000"
    assert normalize_pack_code("XRC-UUUUUUUU") is None  # U is not in Crockford base32
    assert normalize_pack_code("XRC-ABC") is None
