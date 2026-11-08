"""Invite links, knocks, knock-based invites and hole punching (DESIGN.md 10.12, 10.13)."""

import asyncio
import uuid
from datetime import timedelta

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import update

from app.models import Invite, InviteLink, Knock, utcnow
from tests.helpers import (
    AppFactory,
    TestUser,
    befriend,
    client_for,
    register,
    services_of,
    strict_rules,
    ws_login,
)


async def _link(client: httpx.AsyncClient, user: TestUser, mode: str = "zm") -> dict:
    response = await client.post("/v1/links", json={"mode": mode}, headers=user.headers)
    assert response.status_code == 201, response.text
    return response.json()


async def _knock(client: httpx.AsyncClient, user: TestUser, code: str) -> httpx.Response:
    return await client.post(f"/v1/links/{code}/knock", headers=user.headers)


async def _incoming(client: httpx.AsyncClient, user: TestUser) -> list[dict]:
    return (await client.get("/v1/knocks", headers=user.headers)).json()["incoming"]


async def _invite(client: httpx.AsyncClient, host: TestUser, to: TestUser) -> httpx.Response:
    return await client.post(
        "/v1/invites",
        json={"to_user_id": to.id, "session": "xerces/1 8.8.8.8:27017 zm 0"},
        headers=host.headers,
    )


async def test_link_lifecycle(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    first = await _link(client, host, "mp")
    assert first["code"].startswith("XJN-") and len(first["code"]) == 12
    assert first["path"] == f"/join/{first['code']}"
    assert first["mode"] == "mp"
    assert (await client.get("/v1/links", headers=host.headers)).json()["link"] == first

    # A new link replaces the old one.
    second = await _link(client, host)
    assert second["code"] != first["code"]
    guest = await register(client, "guest")
    assert (await client.get(f"/v1/links/{first['code']}", headers=guest.headers)).status_code == 404
    info = await client.get(f"/v1/links/{second['code'].lower()}", headers=guest.headers)
    assert info.status_code == 200
    assert info.json()["host_username"] == "host" and info.json()["host_user_id"] == host.id

    assert (await client.delete("/v1/links", headers=host.headers)).status_code == 204
    assert (await client.get("/v1/links", headers=host.headers)).json() == {"link": None}
    assert (await client.get(f"/v1/links/{second['code']}", headers=guest.headers)).status_code == 404


async def test_links_need_an_account(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    link = await _link(client, host)
    assert (await client.get(f"/v1/links/{link['code']}")).status_code == 401
    assert (await client.post(f"/v1/links/{link['code']}/knock")).status_code == 401
    assert (await client.post("/v1/links", json={})).status_code == 401


async def test_expired_links_are_gone(client: httpx.AsyncClient, app: FastAPI) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host)
    async with services_of(app).sessionmaker() as db:
        await db.execute(update(InviteLink).values(expires_at=utcnow() - timedelta(seconds=1)))
        await db.commit()
    assert (await client.get(f"/v1/links/{link['code']}", headers=guest.headers)).status_code == 404
    assert (await _knock(client, guest, link["code"])).status_code == 404
    assert (await client.get("/v1/links", headers=host.headers)).json() == {"link": None}
    page = await client.get(f"/join/{link['code']}")
    assert page.status_code == 404 and "expired" in page.text


async def test_knock_accept_invite_and_join(client: httpx.AsyncClient, app: FastAPI) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host, "zm")
    host_ws, _ = await ws_login(app, host)
    try:
        knocked = await _knock(client, guest, link["code"])
        assert knocked.status_code == 201, knocked.text
        knock = knocked.json()
        assert knock["from_username"] == "guest" and knock["to_username"] == "host"
        assert knock["mode"] == "zm" and knock["status"] == "pending"
        push = await host_ws.receive_until("knock")
        assert push["id"] == knock["id"] and push["from_username"] == "guest"
    finally:
        await host_ws.close()

    # Not friends, and no knock accepted yet: no invite.
    assert (await _invite(client, host, guest)).status_code == 403
    assert [k["id"] for k in await _incoming(client, host)] == [knock["id"]]
    outgoing = (await client.get("/v1/knocks", headers=guest.headers)).json()["outgoing"]
    assert [k["id"] for k in outgoing] == [knock["id"]]

    # Only the host answers.
    assert (await client.post(f"/v1/knocks/{knock['id']}/accept", headers=guest.headers)).status_code == 404
    accepted = await client.post(f"/v1/knocks/{knock['id']}/accept", headers=host.headers)
    assert accepted.status_code == 200 and accepted.json()["status"] == "accepted"
    assert await _incoming(client, host) == []

    invite = await _invite(client, host, guest)
    assert invite.status_code == 201, invite.text
    taken = await client.post(f"/v1/invites/{invite.json()['id']}/accept", headers=guest.headers)
    assert taken.status_code == 200 and taken.json()["status"] == "accepted"
    # The other way round needs a knock of its own.
    assert (await _invite(client, guest, host)).status_code == 403


async def test_knock_invites_ignore_the_invite_setting(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    patched = await client.patch("/v1/me/privacy", json={"invites": "nobody"}, headers=guest.headers)
    assert patched.status_code == 200, patched.text
    link = await _link(client, host)
    knock = (await _knock(client, guest, link["code"])).json()
    await client.post(f"/v1/knocks/{knock['id']}/accept", headers=host.headers)
    assert (await _invite(client, host, guest)).status_code == 201


async def test_accepted_knocks_allow_invites_for_an_hour(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host)
    knock = (await _knock(client, guest, link["code"])).json()
    accepted = (await client.post(f"/v1/knocks/{knock['id']}/accept", headers=host.headers)).json()
    left = utcnow() + timedelta(hours=1) - httpx_time(accepted["expires_at"])
    assert abs(left.total_seconds()) < 60
    async with services_of(app).sessionmaker() as db:
        await db.execute(update(Knock).values(expires_at=utcnow() - timedelta(seconds=1)))
        await db.commit()
    assert (await _invite(client, host, guest)).status_code == 403


def httpx_time(value: str):
    from datetime import datetime

    return datetime.fromisoformat(value.replace("Z", "+00:00"))


async def test_declined_knocks_stay_quiet(client: httpx.AsyncClient, app: FastAPI) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host)
    knock = (await _knock(client, guest, link["code"])).json()
    assert (await client.post(f"/v1/knocks/{knock['id']}/decline", headers=host.headers)).status_code == 204
    assert await _incoming(client, host) == []
    # The one who knocked still reads "pending", and knocking again neither notifies the host
    # nor brings the knock back.
    outgoing = (await client.get("/v1/knocks", headers=guest.headers)).json()["outgoing"]
    assert [k["status"] for k in outgoing] == ["pending"]
    host_ws, _ = await ws_login(app, host)
    try:
        await asyncio.sleep(0.05)
        again = await _knock(client, guest, link["code"])
        assert again.status_code == 201 and again.json()["id"] == knock["id"]
        assert again.json()["status"] == "pending"
        await host_ws.ping()
    finally:
        await host_ws.close()
    assert await _incoming(client, host) == []
    assert (await _invite(client, host, guest)).status_code == 403


async def test_knocks_across_a_block_go_nowhere(client: httpx.AsyncClient, app: FastAPI) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host)
    blocked = await client.post("/v1/blocks", json={"username": "guest"}, headers=host.headers)
    assert blocked.status_code in (200, 201, 204), blocked.text
    # The blocked side cannot even see whose link it is.
    assert (await client.get(f"/v1/links/{link['code']}", headers=guest.headers)).status_code == 404
    host_ws, _ = await ws_login(app, host)
    try:
        knocked = await _knock(client, guest, link["code"])
        # Answered like any knock, so the block is not revealed...
        assert knocked.status_code == 201 and knocked.json()["status"] == "pending"
        await host_ws.ping()  # ...but no knock reached the host
    finally:
        await host_ws.close()
    assert await _incoming(client, host) == []
    assert (await client.get("/v1/knocks", headers=guest.headers)).json()["outgoing"] == []


async def test_a_block_removes_knocks(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host)
    knock = (await _knock(client, guest, link["code"])).json()
    await client.post(f"/v1/knocks/{knock['id']}/accept", headers=host.headers)
    await client.post("/v1/blocks", json={"username": "host"}, headers=guest.headers)
    await client.delete("/v1/blocks/" + host.id, headers=guest.headers)
    # The accepted knock went with the block, so there is no invite allowance left.
    assert (await _invite(client, host, guest)).status_code == 403


async def test_own_link_and_bad_codes(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    link = await _link(client, host)
    own = await _knock(client, host, link["code"])
    assert own.status_code == 409 and own.json()["error"]["code"] == "own_link"
    for code in ("XJN-00000000", "XRC-00000000", "nonsense", "x" * 32):
        assert (await _knock(client, host, code)).status_code == 404
        assert (await client.get(f"/v1/links/{code}", headers=host.headers)).status_code == 404
    assert (await client.get("/v1/links/" + "x" * 33, headers=host.headers)).status_code == 422


async def test_the_share_page(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    link = await _link(client, host, "zm")
    page = await client.get(f"/join/{link['code']}")
    assert page.status_code == 200
    assert page.headers["content-type"].startswith("text/html")
    assert f'href="xerces://join/{link["code"]}"' in page.text
    assert "Zombies" in page.text
    # It names no one, runs no script and cannot be framed.
    assert "host" not in page.text.replace("the host", "")
    policy = page.headers["content-security-policy"]
    assert "default-src 'none'" in policy and "frame-ancestors 'none'" in policy
    assert "script" not in policy
    assert "<script" not in page.text
    assert page.headers["x-content-type-options"] == "nosniff"


async def test_rate_limits(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("link_create", "knock_host", "join_page"))
    async with client_for(app) as client:
        host = await register(client, "host")
        guest = await register(client, "guest")
        codes = [
            (await client.post("/v1/links", json={}, headers=host.headers)).status_code
            for _ in range(21)
        ]
        assert codes[:20] == [201] * 20 and codes[20] == 429
        link = (await client.get("/v1/links", headers=host.headers)).json()["link"]
        assert (await _knock(client, guest, link["code"])).status_code == 201
        assert (await _knock(client, guest, link["code"])).status_code == 429
        pages = [(await client.get(f"/join/{link['code']}")).status_code for _ in range(61)]
        assert pages[:60] == [200] * 60 and pages[60] == 429


async def test_export_lists_links_and_knocks(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    link = await _link(client, host)
    knock = (await _knock(client, guest, link["code"])).json()
    await client.post(f"/v1/knocks/{knock['id']}/decline", headers=host.headers)
    host_export = (await client.get("/v1/me/export", headers=host.headers)).json()
    assert [entry["code"] for entry in host_export["invite_links"]] == [link["code"]]
    assert host_export["knocks"]["received"][0]["status"] == "declined"
    guest_export = (await client.get("/v1/me/export", headers=guest.headers)).json()
    sent = guest_export["knocks"]["sent"]
    assert sent[0]["to_username"] == "host" and "status" not in sent[0]


# ----------------------------------------------------------------------------- hole punching


async def _accepted_invite(client: httpx.AsyncClient, host: TestUser, guest: TestUser) -> str:
    await befriend(client, host, guest)
    invite = (await _invite(client, host, guest)).json()
    await client.post(f"/v1/invites/{invite['id']}/accept", headers=guest.headers)
    return invite["id"]


async def _punch(
    client: httpx.AsyncClient, user: TestUser, invite_id: str, forwarded: str, port: int = 27017
) -> httpx.Response:
    return await client.post(
        f"/v1/invites/{invite_id}/punch",
        json={"port": port},
        headers={**user.headers, "X-Forwarded-For": forwarded},
    )


async def test_punch_reaches_the_host(client: httpx.AsyncClient, app: FastAPI) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    invite_id = await _accepted_invite(client, host, guest)
    host_ws, _ = await ws_login(app, host)
    try:
        sent = await _punch(client, guest, invite_id, "8.8.8.8", 28960)
        assert sent.status_code == 200, sent.text
        assert sent.json() == {"delivered": True}
        push = await host_ws.receive_until("punch")
        assert push == {
            "type": "punch",
            "invite_id": invite_id,
            "from_user_id": guest.id,
            "address": "8.8.8.8:28960",
        }
        await _punch(client, guest, invite_id, "2001:4860:4860::8888")
        v6 = await host_ws.receive_until("punch")
        assert v6["address"] == "[2001:4860:4860::8888]:27017"
    finally:
        await host_ws.close()
    # With the host offline nothing is delivered.
    assert (await _punch(client, guest, invite_id, "8.8.8.8")).json() == {"delivered": False}


async def test_punch_needs_an_accepted_invite_of_ones_own(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    other = await register(client, "other")
    await befriend(client, host, guest)
    pending = (await _invite(client, host, guest)).json()
    assert (await _punch(client, guest, pending["id"], "8.8.8.8")).status_code == 404
    await client.post(f"/v1/invites/{pending['id']}/accept", headers=guest.headers)
    # Not the host, and not a stranger.
    assert (await _punch(client, host, pending["id"], "8.8.8.8")).status_code == 404
    assert (await _punch(client, other, pending["id"], "8.8.8.8")).status_code == 404
    assert (await _punch(client, guest, pending["id"], "8.8.8.8")).status_code == 200


@pytest.mark.parametrize("port", [0, 65536, -1])
async def test_punch_port_must_be_real(client: httpx.AsyncClient, port: int) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    invite_id = await _accepted_invite(client, host, guest)
    assert (await _punch(client, guest, invite_id, "8.8.8.8", port)).status_code == 422


async def test_punch_without_a_public_address(client: httpx.AsyncClient) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    invite_id = await _accepted_invite(client, host, guest)
    response = await _punch(client, guest, invite_id, "192.168.1.20")
    assert response.status_code == 503 and response.json()["error"]["code"] == "address_unknown"


async def test_punch_is_rate_limited(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("punch"))
    async with client_for(app) as client:
        host = await register(client, "host")
        guest = await register(client, "guest")
        invite_id = await _accepted_invite(client, host, guest)
        codes = [(await _punch(client, guest, invite_id, "8.8.8.8")).status_code for _ in range(31)]
        assert codes[:30] == [200] * 30 and codes[30] == 429


async def test_an_accepted_invite_lasts_an_hour_for_the_punch(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    host = await register(client, "host")
    guest = await register(client, "guest")
    invite_id = await _accepted_invite(client, host, guest)
    async with services_of(app).sessionmaker() as db:
        row = await db.get(Invite, uuid.UUID(invite_id))
        assert row is not None
        left = row.expires_at - utcnow()
        assert timedelta(minutes=59) < left <= timedelta(hours=1)
        row.expires_at = utcnow() - timedelta(seconds=1)
        await db.commit()
    assert (await _punch(client, guest, invite_id, "8.8.8.8")).status_code == 404
