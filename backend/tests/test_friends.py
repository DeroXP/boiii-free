"""Friend requests and friendships."""

from datetime import timedelta

import httpx
from fastapi import FastAPI
from sqlalchemy import func, select, update

from app.models import FriendRequest, Friendship, utcnow
from tests.helpers import TestUser, befriend, register, services_of, ws_login


async def _requests(client: httpx.AsyncClient, user: TestUser) -> dict:
    response = await client.get("/v1/friends/requests", headers=user.headers)
    assert response.status_code == 200
    return response.json()


async def _friends(client: httpx.AsyncClient, user: TestUser) -> list[dict]:
    response = await client.get("/v1/friends", headers=user.headers)
    assert response.status_code == 200
    return response.json()


async def _request_count(app: FastAPI) -> int:
    async with services_of(app).sessionmaker() as db:
        return await db.scalar(select(func.count()).select_from(FriendRequest)) or 0


async def test_request_and_accept(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    sent = await client.post("/v1/friends/requests", json={"username": "BOB"}, headers=alice.headers)
    assert sent.status_code == 202
    assert sent.json() == {"status": "sent"}

    incoming = (await _requests(client, bob))["incoming"]
    assert [(r["user_id"], r["username"]) for r in incoming] == [(alice.id, "alice")]
    outgoing = (await _requests(client, alice))["outgoing"]
    assert [(r["user_id"], r["username"]) for r in outgoing] == [(bob.id, "bob")]

    # Only the recipient can accept.
    wrong = await client.post(f"/v1/friends/requests/{incoming[0]['id']}/accept", headers=alice.headers)
    assert wrong.status_code == 404

    accepted = await client.post(
        f"/v1/friends/requests/{incoming[0]['id']}/accept", headers=bob.headers
    )
    assert accepted.status_code == 200
    assert accepted.json()["user_id"] == alice.id
    assert accepted.json()["username"] == "alice"

    alice_friends = await _friends(client, alice)
    bob_friends = await _friends(client, bob)
    assert [(f["user_id"], f["username"]) for f in alice_friends] == [(bob.id, "bob")]
    assert [(f["user_id"], f["username"]) for f in bob_friends] == [(alice.id, "alice")]
    assert "email" not in alice_friends[0]
    assert await _requests(client, alice) == {"incoming": [], "outgoing": []}
    assert await _requests(client, bob) == {"incoming": [], "outgoing": []}


async def test_request_never_reveals_anything(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    closed = await register(client, "closed")
    await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=closed.headers)

    responses = [
        await client.post("/v1/friends/requests", json={"username": name}, headers=alice.headers)
        for name in ("no-such-user", "closed", "alice", "admin", "x" * 64)
    ]
    for response in responses:
        assert response.status_code == 202
        assert response.json() == {"status": "sent"}
    # The request to "closed" is kept for Alice only; Closed never sees it.
    assert (await _requests(client, closed))["incoming"] == []
    assert [r["username"] for r in (await _requests(client, alice))["outgoing"]] == ["closed"]
    async with services_of(app).sessionmaker() as db:
        assert (await db.scalars(select(FriendRequest.hidden))).all() == [True]


async def test_outgoing_list_gives_away_no_block_or_privacy_setting(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    mallory = await register(client, "mallory")
    open_ = await register(client, "openuser")
    shy = await register(client, "shyuser")
    blocker = await register(client, "blocker")
    await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=shy.headers)
    await client.post("/v1/blocks", json={"username": "mallory"}, headers=blocker.headers)
    sockets = [(await ws_login(app, user))[0] for user in (open_, shy, blocker)]
    try:
        for name in ("openuser", "shyuser", "blocker"):
            sent = await client.post(
                "/v1/friends/requests", json={"username": name}, headers=mallory.headers
            )
            assert sent.status_code == 202 and sent.json() == {"status": "sent"}

        # Three identical answers, and three requests that look the same in Mallory's list.
        outgoing = (await _requests(client, mallory))["outgoing"]
        assert sorted(r["username"] for r in outgoing) == ["blocker", "openuser", "shyuser"]
        assert all(set(r) == {"id", "user_id", "username", "created_at", "expires_at"} for r in outgoing)

        # Only the user who accepts requests from Mallory hears about it.
        open_ws, shy_ws, blocker_ws = sockets
        assert await open_ws.messages_before_pong() == [{"type": "friend_request"}]
        assert await shy_ws.messages_before_pong() == []
        assert await blocker_ws.messages_before_pong() == []
        assert len((await _requests(client, open_))["incoming"]) == 1
        for user in (shy, blocker):
            assert (await _requests(client, user))["incoming"] == []
            hidden_id = next(r["id"] for r in outgoing if r["user_id"] == user.id)
            for action in ("accept", "decline"):
                answer = await client.post(
                    f"/v1/friends/requests/{hidden_id}/{action}", headers=user.headers
                )
                assert answer.status_code == 404
    finally:
        for ws in sockets:
            await ws.close()


async def test_hidden_request_reaches_the_target_when_sent_again_once_allowed(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=bob.headers)
    await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    before = (await _requests(client, alice))["outgoing"]

    await client.patch("/v1/me/privacy", json={"friend_requests": "everyone"}, headers=bob.headers)
    assert (await _requests(client, bob))["incoming"] == []  # a change of mind is not retroactive
    bob_ws, _ = await ws_login(app, bob)
    try:
        await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
        assert await bob_ws.messages_before_pong() == [{"type": "friend_request"}]
    finally:
        await bob_ws.close()
    incoming = (await _requests(client, bob))["incoming"]
    assert [r["user_id"] for r in incoming] == [alice.id]
    # Alice's side did not change at all, so it never showed that it had been hidden.
    assert (await _requests(client, alice))["outgoing"] == before
    accepted = await client.post(f"/v1/friends/requests/{incoming[0]['id']}/accept", headers=bob.headers)
    assert accepted.status_code == 200


async def test_duplicate_request_is_a_noop(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    await register(client, "bob")
    for _ in range(3):
        await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    assert await _request_count(app) == 1


async def test_mutual_requests_become_friends(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    back = await client.post("/v1/friends/requests", json={"username": "alice"}, headers=bob.headers)
    assert back.status_code == 202
    assert back.json() == {"status": "sent"}
    assert [f["user_id"] for f in await _friends(client, alice)] == [bob.id]
    assert await _requests(client, alice) == {"incoming": [], "outgoing": []}
    assert await _requests(client, bob) == {"incoming": [], "outgoing": []}


async def test_cancel_by_sender(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    request_id = (await _requests(client, alice))["outgoing"][0]["id"]

    assert (await client.delete(f"/v1/friends/requests/{request_id}", headers=bob.headers)).status_code == 404
    cancelled = await client.delete(f"/v1/friends/requests/{request_id}", headers=alice.headers)
    assert cancelled.status_code == 204
    assert (await _requests(client, bob))["incoming"] == []


async def test_decline_by_recipient(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    request_id = (await _requests(client, bob))["incoming"][0]["id"]

    assert (
        await client.post(f"/v1/friends/requests/{request_id}/decline", headers=alice.headers)
    ).status_code == 404
    declined = await client.post(f"/v1/friends/requests/{request_id}/decline", headers=bob.headers)
    assert declined.status_code == 204
    assert (await _requests(client, alice))["outgoing"] == []
    gone = await client.post(f"/v1/friends/requests/{request_id}/accept", headers=bob.headers)
    assert gone.status_code == 404
    assert await _friends(client, bob) == []


async def test_remove_friend(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    removed = await client.delete(f"/v1/friends/{bob.id}", headers=alice.headers)
    assert removed.status_code == 204
    assert await _friends(client, alice) == []
    assert await _friends(client, bob) == []
    # Removing again is harmless.
    assert (await client.delete(f"/v1/friends/{bob.id}", headers=alice.headers)).status_code == 204
    async with services_of(app).sessionmaker() as db:
        assert await db.scalar(select(func.count()).select_from(Friendship)) == 0


async def test_expired_requests_are_hidden_and_can_be_resent(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    async with services_of(app).sessionmaker() as db:
        await db.execute(
            update(FriendRequest).values(expires_at=utcnow() - timedelta(seconds=1))
        )
        await db.commit()
    assert (await _requests(client, bob))["incoming"] == []

    again = await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    assert again.status_code == 202
    incoming = (await _requests(client, bob))["incoming"]
    assert len(incoming) == 1
    assert await _request_count(app) == 1


async def test_friend_request_requires_auth(client: httpx.AsyncClient) -> None:
    response = await client.post("/v1/friends/requests", json={"username": "bob"})
    assert response.status_code == 401
