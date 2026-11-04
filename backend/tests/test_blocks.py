"""Blocks cut every tie in both directions."""

import asyncio
import uuid
from collections.abc import Callable
from datetime import timedelta
from typing import Any

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import event, func, select
from sqlalchemy.orm import Session
from sqlalchemy.util import await_only

import app.presence as presence_module
from app.models import FriendRequest, Friendship, Invite, utcnow
from app.routers import blocks as blocks_router
from app.social import ordered_pair
from tests.helpers import TestUser, WSClient, befriend, register, services_of, ws_login


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


def _pause_first_flush_of(kind: type) -> tuple[asyncio.Event, asyncio.Event, Callable[[], None]]:
    """Pause the first write of a new `kind` row just before it reaches the database: the
    request has checked for a block and is about to write. Returns (entered, release, stop)."""
    entered, release = asyncio.Event(), asyncio.Event()

    def pause(session: Session, _context: Any, _instances: Any) -> None:
        if any(isinstance(obj, kind) for obj in session.new) and not release.is_set():
            entered.set()
            await_only(release.wait())

    event.listen(Session, "before_flush", pause)
    return entered, release, lambda: event.remove(Session, "before_flush", pause)


def _pause_statement(
    app: FastAPI, matches: Callable[[str], bool]
) -> tuple[asyncio.Event, asyncio.Event, Callable[[], None]]:
    """Pause the first SQL statement that `matches`, just before it runs, until released.
    Returns (entered, release, stop)."""
    engine = services_of(app).engine.sync_engine
    entered, release = asyncio.Event(), asyncio.Event()

    def pause(_conn: Any, _cursor: Any, statement: str, *_args: Any) -> None:
        if not entered.is_set() and matches(" ".join(statement.split())):
            entered.set()
            await_only(release.wait())

    event.listen(engine, "before_cursor_execute", pause)
    return entered, release, lambda: event.remove(engine, "before_cursor_execute", pause)


async def _incoming(client: httpx.AsyncClient, user: TestUser, what: str) -> list[dict[str, Any]]:
    return (await client.get(f"/v1/{what}", headers=user.headers)).json()["incoming"]


async def test_presence_on_its_way_never_reaches_a_new_blocker(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    alice_ws, _ = await ws_login(app, alice)
    bob_ws, _ = await ws_login(app, bob)
    try:
        assert (await alice_ws.receive_json())["user_id"] == bob.id  # Bob came online
        entered, release = asyncio.Event(), asyncio.Event()
        original = presence_module.block_partner_ids

        async def paused(db: Any, user_id: uuid.UUID) -> set[uuid.UUID]:
            result = await original(db, user_id)
            if user_id == alice.uuid and not release.is_set():
                entered.set()
                await release.wait()  # Alice's fan-out has its audience: Bob, not blocked yet
            return result

        monkeypatch.setattr(presence_module, "block_partner_ids", paused)
        await alice_ws.send_json({"type": "presence", "status": "in_game", "activity": None})
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
        assert blocked.status_code == 204
        release.set()
        assert await alice_ws.messages_before_pong() == [{"type": "offline", "user_id": bob.id}]
        # Bob was told Alice went away, and nothing of hers after that.
        assert await bob_ws.messages_before_pong() == [{"type": "offline", "user_id": alice.id}]
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_friend_request_on_its_way_never_reaches_a_new_blocker(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    mallory = await register(client, "mallory")
    alice = await register(client, "alice")
    alice_ws, _ = await ws_login(app, alice)
    entered, release, stop = _pause_first_flush_of(FriendRequest)
    try:
        sending = asyncio.create_task(
            client.post("/v1/friends/requests", json={"username": "alice"}, headers=mallory.headers)
        )
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "mallory"}, headers=alice.headers)
        assert blocked.status_code == 204
        release.set()
        assert (await sending).status_code == 202
    finally:
        stop()
    try:
        assert await alice_ws.messages_before_pong() == []
    finally:
        await alice_ws.close()
    assert await _incoming(client, alice, "friends/requests") == []
    assert await _count(app, FriendRequest) == 0  # gone, as if sent a moment before the block


async def test_invite_on_its_way_never_reaches_a_new_blocker(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    mallory = await register(client, "mallory")
    await befriend(client, alice, mallory)
    alice_ws, _ = await ws_login(app, alice)
    await alice_ws.messages_before_pong()
    entered, release, stop = _pause_first_flush_of(Invite)
    try:
        inviting = asyncio.create_task(
            client.post(
                "/v1/invites", json={"to_user_id": alice.id, "session": "s1"}, headers=mallory.headers
            )
        )
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "mallory"}, headers=alice.headers)
        assert blocked.status_code == 204
        release.set()
        assert (await inviting).status_code == 403
    finally:
        stop()
    try:
        seen = await alice_ws.messages_before_pong()
        assert [m for m in seen if m["type"] == "invite"] == []
    finally:
        await alice_ws.close()
    assert await _incoming(client, alice, "invites") == []
    assert await _count(app, Invite) == 0


async def test_acceptance_on_its_way_loses_to_a_block(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=bob.headers)
    request_id = (await _incoming(client, alice, "friends/requests"))[0]["id"]
    bob_ws, _ = await ws_login(app, bob)
    entered, release, stop = _pause_first_flush_of(Friendship)
    try:
        accepting = asyncio.create_task(
            client.post(f"/v1/friends/requests/{request_id}/accept", headers=alice.headers)
        )
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
        assert blocked.status_code == 204
        release.set()
        assert (await accepting).status_code == 404
    finally:
        stop()
    try:
        assert await bob_ws.messages_before_pong() == []  # no friend_added, no presence
    finally:
        await bob_ws.close()
    assert await _count(app, Friendship) == 0


async def test_mutual_request_on_its_way_loses_to_a_block(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=bob.headers)
    bob_ws, _ = await ws_login(app, bob)
    entered, release, stop = _pause_first_flush_of(Friendship)
    try:
        # Alice asks Bob back, which makes them friends...
        asking = asyncio.create_task(
            client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
        )
        await asyncio.wait_for(entered.wait(), 3)
        # ...while Bob blocks her.
        blocked = await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
        assert blocked.status_code == 204
        release.set()
        assert (await asking).status_code == 202
    finally:
        stop()
    try:
        assert await bob_ws.messages_before_pong() == []  # no friend_added, no presence
    finally:
        await bob_ws.close()
    assert await _count(app, Friendship) == 0


async def test_request_shown_again_on_its_way_is_not_pushed_to_a_new_blocker(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    mallory = await register(client, "mallory")
    await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=alice.headers)
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=mallory.headers)
    await client.patch("/v1/me/privacy", json={"friend_requests": "everyone"}, headers=alice.headers)
    alice_ws, _ = await ws_login(app, alice)
    # Mallory sends it again, which shows it to Alice; it pauses before its last block check.
    entered, release, stop = _pause_statement(
        app, lambda sql: sql.startswith("SELECT blocks.blocker_id FROM blocks") and "LIMIT" in sql
    )
    try:
        sending = asyncio.create_task(
            client.post("/v1/friends/requests", json={"username": "alice"}, headers=mallory.headers)
        )
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "mallory"}, headers=alice.headers)
        assert blocked.status_code == 204
        release.set()
        assert (await sending).status_code == 202
    finally:
        stop()
    try:
        assert await alice_ws.messages_before_pong() == []
    finally:
        await alice_ws.close()
    assert await _incoming(client, alice, "friends/requests") == []


async def test_new_friends_see_no_presence_once_a_block_comes_in_between(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=bob.headers)
    request_id = (await _incoming(client, alice, "friends/requests"))[0]["id"]
    alice_ws, _ = await ws_login(app, alice)
    bob_ws, _ = await ws_login(app, bob)
    # Alice accepts; the presence each should now see of the other waits on its privacy read.
    entered, release, stop = _pause_statement(
        app, lambda sql: sql.startswith("SELECT users.id FROM users WHERE users.id IN")
    )
    try:
        accepting = asyncio.create_task(
            client.post(f"/v1/friends/requests/{request_id}/accept", headers=alice.headers)
        )
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
        assert blocked.status_code == 204
        release.set()
        assert (await accepting).status_code == 200  # it had been accepted before the block
    finally:
        stop()
    try:
        # Told of the friendship, then that it ended, and nothing after that.
        assert [m["type"] for m in await bob_ws.messages_before_pong()] == ["friend_added", "offline"]
        assert [m["type"] for m in await alice_ws.messages_before_pong()] == [
            "friend_added",
            "offline",
        ]
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_hello_leaves_out_a_friend_who_blocks_meanwhile(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    bob_ws, _ = await ws_login(app, bob)
    # Alice's socket signs in; its hello waits on the read of which friends show presence.
    entered, release, stop = _pause_statement(
        app, lambda sql: sql.startswith("SELECT users.id FROM users WHERE users.id IN")
    )
    alice_ws = await WSClient(app).connect()
    try:
        await alice_ws.send_json({"type": "auth", "token": alice.access_token})
        await asyncio.wait_for(entered.wait(), 3)
        blocked = await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
        assert blocked.status_code == 204
        release.set()
        hello = await alice_ws.receive_until("hello")
        assert hello["friends"] == []  # not Bob, who blocked her before the hello went out
    finally:
        stop()
        await alice_ws.close()
        await bob_ws.close()


async def test_a_block_cuts_again_what_slipped_past_its_first_cut(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    sessionmaker = services_of(app).sessionmaker
    original = blocks_router.cut_ties
    calls = []

    async def cut_ties(db: Any, a: uuid.UUID, b: uuid.UUID, **options: Any) -> bool:
        calls.append(options)
        if len(calls) == 2:
            # What requests that checked for a block before it committed wrote after its cut
            # (on PostgreSQL that can happen; SQLite lets one writer at a time).
            async with sessionmaker() as other:
                now = utcnow()
                low, high = ordered_pair(alice.uuid, bob.uuid)
                later = now + timedelta(days=1)
                other.add(Friendship(user_low=low, user_high=high, created_at=now))
                other.add(
                    FriendRequest(
                        from_user_id=alice.uuid, to_user_id=bob.uuid, created_at=now, expires_at=later
                    )
                )
                # Sent by Bob after the block: hidden from Alice, and kept for him as usual.
                other.add(
                    FriendRequest(
                        from_user_id=bob.uuid,
                        to_user_id=alice.uuid,
                        created_at=now,
                        expires_at=later,
                        hidden=True,
                    )
                )
                other.add(
                    Invite(
                        from_user_id=bob.uuid,
                        to_user_id=alice.uuid,
                        session="s",
                        created_at=now,
                        expires_at=later,
                        status="pending",
                    )
                )
                await other.commit()
        return await original(db, a, b, **options)

    monkeypatch.setattr(blocks_router, "cut_ties", cut_ties)
    blocked = await client.post("/v1/blocks", json={"username": "bob"}, headers=alice.headers)
    assert blocked.status_code == 204
    assert len(calls) == 2
    assert await _count(app, Friendship) == 0
    assert await _count(app, Invite) == 0
    async with sessionmaker() as db:
        left = (await db.execute(select(FriendRequest.from_user_id, FriendRequest.hidden))).all()
    assert [tuple(row) for row in left] == [(bob.uuid, True)]


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
