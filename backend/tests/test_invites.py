"""Invites: friends only, 10 minutes, answered by the recipient only."""

from datetime import timedelta

import httpx
from fastapi import FastAPI
from sqlalchemy import update

from app.models import Invite, utcnow
from tests.helpers import AppFactory, befriend, client_for, register, services_of, strict_rules


async def test_invite_flow(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)

    created = await client.post(
        "/v1/invites",
        json={"to_user_id": bob.id, "session": "opaque-session-1", "pack_code": "xrc-ab12cd34"},
        headers=alice.headers,
    )
    assert created.status_code == 201
    invite = created.json()
    assert invite["from_user_id"] == alice.id and invite["from_username"] == "alice"
    assert invite["to_user_id"] == bob.id and invite["to_username"] == "bob"
    assert invite["status"] == "pending"
    assert invite["pack_code"] == "XRC-AB12CD34"

    bob_view = (await client.get("/v1/invites", headers=bob.headers)).json()
    assert [i["id"] for i in bob_view["incoming"]] == [invite["id"]]
    assert bob_view["outgoing"] == []
    alice_view = (await client.get("/v1/invites", headers=alice.headers)).json()
    assert [i["id"] for i in alice_view["outgoing"]] == [invite["id"]]

    # Only the recipient answers.
    assert (await client.post(f"/v1/invites/{invite['id']}/accept", headers=alice.headers)).status_code == 404
    accepted = await client.post(f"/v1/invites/{invite['id']}/accept", headers=bob.headers)
    assert accepted.status_code == 200
    assert accepted.json()["status"] == "accepted"
    assert accepted.json()["session"] == "opaque-session-1"
    # Answered invites leave the inbox; the sender sees the answer.
    assert (await client.get("/v1/invites", headers=bob.headers)).json()["incoming"] == []
    outgoing = (await client.get("/v1/invites", headers=alice.headers)).json()["outgoing"]
    assert outgoing[0]["status"] == "accepted"
    assert (await client.post(f"/v1/invites/{invite['id']}/decline", headers=bob.headers)).status_code == 404


async def test_decline(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    invite = (
        await client.post("/v1/invites", json={"to_user_id": bob.id, "session": "s"}, headers=alice.headers)
    ).json()
    assert (await client.post(f"/v1/invites/{invite['id']}/decline", headers=alice.headers)).status_code == 404
    declined = await client.post(f"/v1/invites/{invite['id']}/decline", headers=bob.headers)
    assert declined.status_code == 204
    assert (await client.post(f"/v1/invites/{invite['id']}/accept", headers=bob.headers)).status_code == 404


async def test_only_friends(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    stranger = await register(client, "stranger")
    for target in (stranger.id, alice.id, "00000000-0000-0000-0000-000000000000"):
        response = await client.post(
            "/v1/invites", json={"to_user_id": target, "session": "s"}, headers=alice.headers
        )
        assert response.status_code == 403
        assert response.json()["error"] == {
            "code": "forbidden",
            "message": "You cannot invite this user.",
        }


async def test_expired_invites(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    invite = (
        await client.post("/v1/invites", json={"to_user_id": bob.id, "session": "s"}, headers=alice.headers)
    ).json()
    async with services_of(app).sessionmaker() as db:
        await db.execute(update(Invite).values(expires_at=utcnow() - timedelta(seconds=1)))
        await db.commit()
    assert (await client.get("/v1/invites", headers=bob.headers)).json() == {
        "incoming": [],
        "outgoing": [],
    }
    assert (await client.post(f"/v1/invites/{invite['id']}/accept", headers=bob.headers)).status_code == 404


async def test_invite_expiry_is_ten_minutes(client: httpx.AsyncClient) -> None:
    from datetime import datetime

    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    invite = (
        await client.post("/v1/invites", json={"to_user_id": bob.id, "session": "s"}, headers=alice.headers)
    ).json()
    created = datetime.fromisoformat(invite["created_at"])
    expires = datetime.fromisoformat(invite["expires_at"])
    assert expires - created == timedelta(minutes=10)


async def test_unfriending_removes_invites(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    invite = (
        await client.post("/v1/invites", json={"to_user_id": bob.id, "session": "s"}, headers=alice.headers)
    ).json()
    await client.delete(f"/v1/friends/{alice.id}", headers=bob.headers)
    assert (await client.post(f"/v1/invites/{invite['id']}/accept", headers=bob.headers)).status_code == 404


async def test_invite_validation(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    for body in (
        {"to_user_id": bob.id, "session": ""},
        {"to_user_id": bob.id, "session": "x" * 257},
        {"to_user_id": bob.id, "session": "bad\x00session"},
        {"to_user_id": bob.id, "session": "s", "pack_code": "not-a-code"},
        {"to_user_id": "not-a-uuid", "session": "s"},
    ):
        response = await client.post("/v1/invites", json=body, headers=alice.headers)
        assert response.status_code == 422, body


async def test_one_invite_per_recipient_per_30_seconds(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("invite_recipient"))
    async with client_for(app) as client:
        alice = await register(client, "alice")
        bob = await register(client, "bob")
        carol = await register(client, "carol")
        await befriend(client, alice, bob)
        await befriend(client, alice, carol)
        first = await client.post(
            "/v1/invites", json={"to_user_id": bob.id, "session": "s"}, headers=alice.headers
        )
        assert first.status_code == 201
        second = await client.post(
            "/v1/invites", json={"to_user_id": bob.id, "session": "s"}, headers=alice.headers
        )
        assert second.status_code == 429
        assert 0 < int(second.headers["retry-after"]) <= 30
        other = await client.post(
            "/v1/invites", json={"to_user_id": carol.id, "session": "s"}, headers=alice.headers
        )
        assert other.status_code == 201
