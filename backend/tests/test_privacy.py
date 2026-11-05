"""Privacy settings: who can send requests, see presence and invite."""

import httpx
import pytest
from fastapi import FastAPI

from tests.helpers import befriend, register, ws_login


async def test_privacy_defaults_and_update(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    updated = await client.patch(
        "/v1/me/privacy", json={"presence": "nobody", "invites": "nobody"}, headers=alice.headers
    )
    assert updated.status_code == 200
    assert updated.json()["privacy"] == {
        "friend_requests": "everyone",
        "presence": "nobody",
        "invites": "nobody",
    }
    bad = await client.patch("/v1/me/privacy", json={"presence": "everyone"}, headers=alice.headers)
    assert bad.status_code == 422
    unknown = await client.patch("/v1/me/privacy", json={"colour": "red"}, headers=alice.headers)
    assert unknown.status_code == 422


async def test_friend_requests_nobody(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    carol = await register(client, "carol")
    await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=bob.headers)
    sent = await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
    assert sent.status_code == 202
    assert sent.json() == {"status": "sent"}
    incoming = (await client.get("/v1/friends/requests", headers=bob.headers)).json()["incoming"]
    assert incoming == []
    # Bob can still ask people himself.
    await client.post("/v1/friends/requests", json={"username": "carol"}, headers=bob.headers)
    assert len((await client.get("/v1/friends/requests", headers=carol.headers)).json()["incoming"]) == 1
    # Alice had asked him (he never saw it), so his request to her is a yes, as for any request.
    # Alice cannot tell from this that hers had been hidden.
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=bob.headers)
    friends = (await client.get("/v1/friends", headers=alice.headers)).json()
    assert [f["user_id"] for f in friends] == [bob.id]
    assert (await client.get("/v1/friends/requests", headers=alice.headers)).json() == {
        "incoming": [],
        "outgoing": [],
    }


async def test_presence_nobody_hides_presence(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    await client.patch("/v1/me/privacy", json={"presence": "nobody"}, headers=bob.headers)

    alice_ws, _ = await ws_login(app, alice)
    bob_ws, bob_hello = await ws_login(app, bob)
    try:
        # Bob still sees Alice (her presence is visible to friends).
        assert [f["user_id"] for f in bob_hello["friends"]] == [alice.id]
        await bob_ws.send_json({"type": "presence", "status": "in_game", "activity": None})
        await bob_ws.ping()
        # Nothing about Bob reaches Alice: her next message is the pong.
        await alice_ws.ping()
        friends = (await client.get("/v1/friends", headers=alice.headers)).json()
        assert friends[0]["user_id"] == bob.id
        assert friends[0]["presence"] is None
        bob_view = (await client.get("/v1/friends", headers=bob.headers)).json()
        assert bob_view[0]["presence"] == {"status": "online", "activity": None}
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_presence_privacy_change_takes_effect_live(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    alice_ws, _ = await ws_login(app, alice)
    bob_ws, _ = await ws_login(app, bob)
    try:
        assert (await alice_ws.receive_json())["type"] == "presence"  # Bob came online
        await client.patch("/v1/me/privacy", json={"presence": "nobody"}, headers=bob.headers)
        assert await alice_ws.receive_json() == {"type": "offline", "user_id": bob.id}
        await client.patch("/v1/me/privacy", json={"presence": "friends"}, headers=bob.headers)
        back = await alice_ws.receive_json()
        assert back["type"] == "presence" and back["user_id"] == bob.id
    finally:
        await alice_ws.close()
        await bob_ws.close()


@pytest.mark.parametrize("presence", ["friends", "nobody"])
async def test_blocking_a_stranger_reveals_nothing(
    client: httpx.AsyncClient, app: FastAPI, presence: str
) -> None:
    alice = await register(client, "alice")
    mallory = await register(client, "mallory")  # not Alice's friend
    await client.patch("/v1/me/privacy", json={"presence": presence}, headers=alice.headers)
    alice_ws, _ = await ws_login(app, alice)
    mallory_ws, _ = await ws_login(app, mallory)
    try:
        # A block and unblock must not answer "is Alice online right now?", nor reach Alice.
        for _ in range(2):
            blocked = await client.post(
                "/v1/blocks", json={"username": "alice"}, headers=mallory.headers
            )
            assert blocked.status_code == 204
            unblocked = await client.delete(f"/v1/blocks/{alice.id}", headers=mallory.headers)
            assert unblocked.status_code == 204
            assert await mallory_ws.messages_before_pong() == []
            assert await alice_ws.messages_before_pong() == []
    finally:
        await alice_ws.close()
        await mallory_ws.close()


async def test_unfriending_tells_only_those_who_could_see(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")  # hides her presence
    bob = await register(client, "bob")  # shows his to friends
    carol = await register(client, "carol")  # hides hers too
    await befriend(client, alice, bob)
    await befriend(client, carol, bob)
    for hidden in (alice, carol):
        await client.patch("/v1/me/privacy", json={"presence": "nobody"}, headers=hidden.headers)
    bob_ws, _ = await ws_login(app, bob)
    alice_ws, _ = await ws_login(app, alice)
    carol_ws, _ = await ws_login(app, carol)
    try:
        for ws in (alice_ws, carol_ws):  # each saw Bob, who was already online
            assert await ws.messages_before_pong() == []
        assert await bob_ws.messages_before_pong() == []  # Bob never saw them come online

        # Unfriending: Bob learns nothing about Alice; Alice could see Bob, so she sees him go.
        assert (await client.delete(f"/v1/friends/{alice.id}", headers=bob.headers)).status_code == 204
        assert await bob_ws.messages_before_pong() == []
        assert await alice_ws.messages_before_pong() == [{"type": "offline", "user_id": bob.id}]

        # Blocking a friend: the same rule.
        blocked = await client.post("/v1/blocks", json={"username": "carol"}, headers=bob.headers)
        assert blocked.status_code == 204
        assert await bob_ws.messages_before_pong() == []
        assert await carol_ws.messages_before_pong() == [{"type": "offline", "user_id": bob.id}]
    finally:
        await alice_ws.close()
        await bob_ws.close()
        await carol_ws.close()


async def test_invites_nobody(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    await client.patch("/v1/me/privacy", json={"invites": "nobody"}, headers=bob.headers)
    response = await client.post(
        "/v1/invites", json={"to_user_id": bob.id, "session": "lobby"}, headers=alice.headers
    )
    assert response.status_code == 403
    assert response.json() == {
        "error": {"code": "forbidden", "message": "You cannot invite this user."}
    }
