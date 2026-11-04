"""The account itself: export and deletion."""

from datetime import timedelta

import httpx
from fastapi import FastAPI
from sqlalchemy import func, select, update

from app.models import (
    Block,
    DeletedUsername,
    FriendRequest,
    Friendship,
    Invite,
    Pack,
    RefreshToken,
    Report,
    User,
    utcnow,
)
from tests.helpers import PASSWORD, befriend, register, services_of, ws_login


async def _count(app: FastAPI, model: type, *where) -> int:
    async with services_of(app).sessionmaker() as db:
        return await db.scalar(select(func.count()).select_from(model).where(*where)) or 0


async def _populate(client: httpx.AsyncClient):
    alice = await register(client, "alice", email="alice@example.com")
    bob = await register(client, "bob", email="bob-secret@example.com")
    carol = await register(client, "carol")
    dave = await register(client, "dave")
    await befriend(client, alice, bob)
    await client.post("/v1/friends/requests", json={"username": "carol"}, headers=alice.headers)
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=dave.headers)
    await client.post("/v1/blocks", json={"username": "dave"}, headers=alice.headers)
    await client.post(
        "/v1/reports", json={"username": "carol", "reason": "spam", "details": "ads"}, headers=alice.headers
    )
    await client.post(
        "/v1/reports", json={"username": "alice", "reason": "cheating"}, headers=bob.headers
    )
    await client.post(
        "/v1/invites", json={"to_user_id": bob.id, "session": "lobby-1"}, headers=alice.headers
    )
    await client.post(
        "/v1/invites", json={"to_user_id": alice.id, "session": "lobby-2"}, headers=bob.headers
    )
    created = await client.post(
        "/v1/packs",
        json={"pack": {"schema": 1, "name": "Mine", "mods": [{"id": "ws.1944930083"}]}},
        headers=alice.headers,
    )
    assert created.status_code == 201
    return alice, bob, carol, dave


async def test_me(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice", email="alice@example.com")
    body = (await client.get("/v1/me", headers=alice.headers)).json()
    assert body["username"] == "alice"
    assert body["email"] == "alice@example.com"
    assert set(body) == {"id", "username", "email", "privacy", "created_at"}


async def test_export_contents(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice, _bob, _carol, _dave = await _populate(client)
    response = await client.get("/v1/me/export", headers=alice.headers)
    assert response.status_code == 200
    data = response.json()
    text = response.text

    assert data["user"]["username"] == "alice"
    assert data["user"]["email"] == "alice@example.com"
    assert data["user"]["tokens_valid_after"] is None
    assert "password_hash" not in text and "$argon2" not in text
    assert "token_hash" not in text
    assert alice.refresh_token not in text
    # Other users' private data stays out: Bob's email.
    assert "bob-secret@example.com" not in text
    # Bob's report about Alice is hers to see, but not that Bob filed it, nor its exact time.
    async with services_of(app).sessionmaker() as db:
        filed = await db.scalar(select(Report.created_at).where(Report.reported_id == alice.uuid))
    assert filed is not None
    assert data["reports_about_you"] == [
        {"reason": "cheating", "status": "open", "created_on": filed.date().isoformat()}
    ]

    assert [f["username"] for f in data["friends"]] == ["bob"]
    assert [r["to_username"] for r in data["friend_requests"]["outgoing"]] == ["carol"]
    assert data["friend_requests"]["incoming"] == []  # Dave's request died with the block
    assert [b["username"] for b in data["blocks"]] == ["dave"]
    assert [(r["reported_username"], r["reason"], r["details"]) for r in data["reports_filed"]] == [
        ("carol", "spam", "ads")
    ]
    assert [i["session"] for i in data["invites"]["sent"]] == ["lobby-1"]
    assert [i["session"] for i in data["invites"]["received"]] == ["lobby-2"]
    assert [p["pack"]["name"] for p in data["packs"]] == ["Mine"]
    assert len(data["sessions"]) >= 1 and set(data["sessions"][0]) == {
        "id",
        "family_id",
        "created_at",
        "expires_at",
        "revoked_at",
    }


async def test_delete_account(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice, bob, _carol, _dave = await _populate(client)
    alice_id = alice.uuid

    wrong = await client.request(
        "DELETE", "/v1/me", json={"password": "not the password"}, headers=alice.headers
    )
    assert wrong.status_code == 403
    deleted = await client.request(
        "DELETE", "/v1/me", json={"password": alice.password}, headers=alice.headers
    )
    assert deleted.status_code == 204

    assert await _count(app, User, User.id == alice_id) == 0
    assert await _count(app, RefreshToken, RefreshToken.user_id == alice_id) == 0
    assert await _count(app, Friendship) == 0
    assert await _count(app, FriendRequest) == 0
    assert await _count(app, Block) == 0
    assert await _count(app, Invite) == 0
    assert await _count(app, Pack) == 0
    # Reports survive with the link to Alice removed, on both sides.
    async with services_of(app).sessionmaker() as db:
        reports = (await db.scalars(select(Report).order_by(Report.reason))).all()
    assert [(r.reason, r.reporter_id is None, r.reported_id is None) for r in reports] == [
        ("cheating", False, True),
        ("spam", True, False),
    ]

    assert (await client.get("/v1/me", headers=alice.headers)).status_code == 401
    login = await client.post("/v1/auth/login", json={"username": "alice", "password": alice.password})
    assert login.status_code == 401
    refresh = await client.post("/v1/auth/refresh", json={"refresh_token": alice.refresh_token})
    assert refresh.status_code == 401
    assert (await client.get("/v1/friends", headers=bob.headers)).json() == []


async def test_deleted_username_is_held_for_90_days(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "Alice")
    deleted = await client.request(
        "DELETE", "/v1/me", json={"password": alice.password}, headers=alice.headers
    )
    assert deleted.status_code == 204

    # Nobody can take the name over and pass as the person who left, in any letter case.
    for name in ("Alice", "alice", "ALICE"):
        again = await client.post("/v1/auth/register", json={"username": name, "password": PASSWORD})
        assert again.status_code == 409
        assert again.json()["error"]["code"] == "username_taken"
    async with services_of(app).sessionmaker() as db:
        holds = (await db.scalars(select(DeletedUsername))).all()
        assert [hold.username_lower for hold in holds] == ["alice"]  # the name only, no account link
        held_for = holds[0].expires_at - utcnow()
        assert timedelta(days=89, hours=23) < held_for <= timedelta(days=90)
        # 90 days later the name is free again.
        await db.execute(update(DeletedUsername).values(expires_at=utcnow() - timedelta(seconds=1)))
        await db.commit()
    assert (await register(client, "alice")).username == "alice"


async def test_export_marks_hidden_requests_for_the_recipient_only(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    carol = await register(client, "carol")
    await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=alice.headers)
    await client.post("/v1/friends/requests", json={"username": "alice"}, headers=carol.headers)

    mine = (await client.get("/v1/me/export", headers=alice.headers)).json()
    assert [(r["from_username"], r["hidden"]) for r in mine["friend_requests"]["incoming"]] == [
        ("carol", True)
    ]
    # The sender's copy must not say it was hidden: that would tell Carol about Alice's setting.
    theirs = (await client.get("/v1/me/export", headers=carol.headers)).json()
    outgoing = theirs["friend_requests"]["outgoing"]
    assert [r["to_username"] for r in outgoing] == ["alice"]
    assert "hidden" not in outgoing[0]


async def test_delete_account_closes_websocket(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    bob_ws, _ = await ws_login(app, bob)
    alice_ws, _ = await ws_login(app, alice)
    try:
        assert (await bob_ws.receive_json())["type"] == "presence"
        deleted = await client.request(
            "DELETE", "/v1/me", json={"password": alice.password}, headers=alice.headers
        )
        assert deleted.status_code == 204
        assert await alice_ws.expect_closed() == 4410
        assert await bob_ws.receive_json() == {"type": "offline", "user_id": alice.id}
    finally:
        await alice_ws.close()
        await bob_ws.close()
