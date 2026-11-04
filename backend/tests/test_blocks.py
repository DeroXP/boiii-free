"""Blocks cut every tie in both directions."""

import httpx
from fastapi import FastAPI
from sqlalchemy import func, select

from app.models import FriendRequest, Invite
from tests.helpers import befriend, register, services_of, ws_login


async def _count(app: FastAPI, model: type) -> int:
    async with services_of(app).sessionmaker() as db:
        return await db.scalar(select(func.count()).select_from(model)) or 0


async def test_block_removes_friendship_requests_and_invites(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    invite = await client.post(
        "/v1/invites", json={"to_user_id": bob.id, "session": "s1"}, headers=alice.headers
    )
    assert invite.status_code == 201
    invite_back = await client.post(
        "/v1/invites", json={"to_user_id": alice.id, "session": "s2"}, headers=bob.headers
    )
    assert invite_back.status_code == 201

    blocked = await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
    assert blocked.status_code == 204
    assert (await client.get("/v1/friends", headers=alice.headers)).json() == []
    assert (await client.get("/v1/friends", headers=bob.headers)).json() == []
    assert await _count(app, Invite) == 0

    listed = (await client.get("/v1/blocks", headers=bob.headers)).json()
    assert [(b["user_id"], b["username"]) for b in listed] == [(alice.id, "alice")]
    assert (await client.get("/v1/blocks", headers=alice.headers)).json() == []


async def test_block_removes_pending_requests(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    assert await _count(app, FriendRequest) == 1
    await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
    assert await _count(app, FriendRequest) == 0


async def test_blocked_users_cannot_request_or_invite(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/blocks", json={"username": "bob"}, headers=alice.headers)
    alice_ws, _ = await ws_login(app, alice)

    # Both directions: still 202 (nothing revealed), and nothing reaches the other side.
    for sender, target in ((bob, "alice"), (alice, "bob")):
        response = await client.post(
            "/v1/friends/requests", json={"username": target}, headers=sender.headers
        )
        assert response.status_code == 202
    try:
        assert await alice_ws.messages_before_pong() == []  # no friend_request push
    finally:
        await alice_ws.close()
    assert (await client.get("/v1/friends/requests", headers=alice.headers)).json() == {
        "incoming": [],
        "outgoing": [],
    }
    # Bob's request is kept for him alone, so his list looks as if Alice has not answered yet.
    bob_view = (await client.get("/v1/friends/requests", headers=bob.headers)).json()
    assert bob_view["incoming"] == []
    assert [r["username"] for r in bob_view["outgoing"]] == ["alice"]
    async with services_of(app).sessionmaker() as db:
        assert (await db.scalars(select(FriendRequest.hidden))).all() == [True]

    for sender, target in ((bob, alice), (alice, bob)):
        response = await client.post(
            "/v1/invites", json={"to_user_id": target.id, "session": "s"}, headers=sender.headers
        )
        assert response.status_code == 403
        assert response.json()["error"]["message"] == "You cannot invite this user."


async def test_unblock(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/blocks", json={"username": "bob"}, headers=alice.headers)
    unblocked = await client.delete(f"/v1/blocks/{bob.id}", headers=alice.headers)
    assert unblocked.status_code == 204
    assert (await client.get("/v1/blocks", headers=alice.headers)).json() == []
    await befriend(client, alice, bob)
    assert len((await client.get("/v1/friends", headers=alice.headers)).json()) == 1


async def test_block_edge_cases(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    unknown = await client.post("/v1/blocks", json={"username": "ghost"}, headers=alice.headers)
    assert unknown.status_code == 204
    yourself = await client.post("/v1/blocks", json={"username": "alice"}, headers=alice.headers)
    assert yourself.status_code == 422
    await register(client, "bob")
    for _ in range(2):  # blocking twice is fine
        again = await client.post("/v1/blocks", json={"username": "bob"}, headers=alice.headers)
        assert again.status_code == 204
    assert len((await client.get("/v1/blocks", headers=alice.headers)).json()) == 1
