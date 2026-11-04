"""WebSocket /v1/ws: auth handshake, presence fan-out and pushes."""

import asyncio
import json
import logging
import time
from typing import Any

import httpx
import pytest
from fastapi import FastAPI

import app.presence as presence_module
from app.log import JsonFormatter
from app.routers import ws as ws_router
from app.tokens import ACCESS_TOKEN_TTL_SECONDS
from tests.helpers import (
    AppFactory,
    TestUser,
    WSClient,
    WSClosed,
    befriend,
    client_for,
    register,
    services_of,
    strict_rules,
    ws_login,
)


async def _connect_with(app: FastAPI, token: str) -> WSClient:
    ws = await WSClient(app).connect()
    await ws.send_json({"type": "auth", "token": token})
    hello = await ws.receive_json()
    assert hello["type"] == "hello", hello
    return ws


def _token_expiring_soon(app: FastAPI, user: TestUser) -> str:
    """An access token with one to two seconds left."""
    expires = int(time.time()) + 2
    return services_of(app).tokens.issue(user.uuid, now=expires - ACCESS_TOKEN_TTL_SECONDS)


async def test_first_frame_must_be_auth(app: FastAPI) -> None:
    async with WSClient(app) as ws:
        await ws.send_json({"type": "ping"})
        assert await ws.expect_closed() == 1008


async def test_garbage_first_frame(app: FastAPI) -> None:
    async with WSClient(app) as ws:
        await ws.send_text("not json")
        assert await ws.expect_closed() == 1008


async def test_invalid_token(app: FastAPI) -> None:
    async with WSClient(app) as ws:
        await ws.send_json({"type": "auth", "token": "nope"})
        assert await ws.expect_closed() == 4401


async def test_auth_timeout(app_factory: AppFactory) -> None:
    app = await app_factory(ws_auth_timeout=0.2)
    async with WSClient(app) as ws:
        assert await ws.expect_closed(timeout=3) == 1008


async def test_hello_ping_and_bad_messages(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    ws, hello = await ws_login(app, alice)
    try:
        assert hello == {"type": "hello", "user_id": alice.id, "friends": []}
        await ws.ping()
        await ws.send_text("[1, 2]")
        assert (await ws.receive_json())["code"] == "invalid_message"
        await ws.send_json({"type": "presence", "status": "dancing"})
        assert (await ws.receive_json())["code"] == "invalid_presence"
        await ws.send_json({"type": "presence", "status": "online", "activity": {"mode": "zm", "joinable": "yes"}})
        assert (await ws.receive_json())["code"] == "invalid_presence"
        await ws.send_json({"type": "teleport"})
        assert (await ws.receive_json())["code"] == "unknown_type"
        await ws.ping()
    finally:
        await ws.close()


async def test_presence_reaches_friends_only(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    carol = await register(client, "carol")  # not a friend
    await befriend(client, alice, bob)

    bob_ws, bob_hello = await ws_login(app, bob)
    carol_ws, _ = await ws_login(app, carol)
    alice_ws, alice_hello = await ws_login(app, alice)
    try:
        assert bob_hello["friends"] == []
        assert alice_hello["friends"] == [{"user_id": bob.id, "status": "online", "activity": None}]
        assert await bob_ws.receive_json() == {
            "type": "presence",
            "user_id": alice.id,
            "status": "online",
            "activity": None,
        }

        await alice_ws.send_json(
            {
                "type": "presence",
                "status": "in_game",
                "activity": {"mode": "zm", "joinable": True, "pack_code": "xrc-ab12cd34"},
            }
        )
        update = await bob_ws.receive_json()
        assert update == {
            "type": "presence",
            "user_id": alice.id,
            "status": "in_game",
            "activity": {"mode": "zm", "joinable": True, "pack_code": "XRC-AB12CD34"},
        }
        await alice_ws.ping()
        # Carol saw nothing: her next message is the answer to her own ping.
        await carol_ws.ping()

        friends = (await client.get("/v1/friends", headers=bob.headers)).json()
        assert friends[0]["presence"]["status"] == "in_game"

        await alice_ws.close()
        assert await bob_ws.receive_json() == {"type": "offline", "user_id": alice.id}
        await carol_ws.ping()
    finally:
        await alice_ws.close()
        await bob_ws.close()
        await carol_ws.close()


async def test_blocked_users_never_see_presence(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    alice_ws, _ = await ws_login(app, alice)
    bob_ws, _ = await ws_login(app, bob)
    try:
        assert (await alice_ws.receive_json())["user_id"] == bob.id  # Bob came online

        await client.post("/v1/blocks", json={"username": "alice"}, headers=bob.headers)
        # The block hides each from the other right away...
        assert await alice_ws.receive_json() == {"type": "offline", "user_id": bob.id}
        assert await bob_ws.receive_json() == {"type": "offline", "user_id": alice.id}
        # ...and nothing flows afterwards, in either direction.
        await alice_ws.send_json({"type": "presence", "status": "away"})
        await bob_ws.send_json({"type": "presence", "status": "in_game"})
        await alice_ws.ping()
        await bob_ws.ping()
        await alice_ws.ping()
        await bob_ws.ping()
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_invite_push(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    bob_ws, _ = await ws_login(app, bob)
    try:
        created = await client.post(
            "/v1/invites",
            json={"to_user_id": bob.id, "session": "lobby-42", "pack_code": "XRC-AB12CD34"},
            headers=alice.headers,
        )
        assert created.status_code == 201
        push = await bob_ws.receive_until("invite")
        assert push["id"] == created.json()["id"]
        assert push["from_user_id"] == alice.id
        assert push["from_username"] == "alice"
        assert push["session"] == "lobby-42"
        assert push["pack_code"] == "XRC-AB12CD34"
    finally:
        await bob_ws.close()


async def test_friend_request_and_friend_added_pushes(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    alice_ws, _ = await ws_login(app, alice)
    bob_ws, _ = await ws_login(app, bob)
    try:
        await client.post("/v1/friends/requests", json={"username": "bob"}, headers=alice.headers)
        assert await bob_ws.receive_json() == {"type": "friend_request"}

        request_id = (await client.get("/v1/friends/requests", headers=bob.headers)).json()[
            "incoming"
        ][0]["id"]
        await client.post(f"/v1/friends/requests/{request_id}/accept", headers=bob.headers)
        assert await alice_ws.receive_json() == {
            "type": "friend_added",
            "user_id": bob.id,
            "username": "bob",
        }
        presence = await alice_ws.receive_json()
        assert presence["type"] == "presence" and presence["user_id"] == bob.id
        assert (await bob_ws.receive_json())["type"] == "friend_added"
        assert (await bob_ws.receive_json())["user_id"] == alice.id
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_new_connection_replaces_old(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    bob_ws, _ = await ws_login(app, bob)
    first, _ = await ws_login(app, alice)
    second, _ = await ws_login(app, alice)
    try:
        assert await first.expect_closed() == 4000
        # Bob saw Alice come online twice, and never go offline in between.
        assert (await bob_ws.receive_json())["type"] == "presence"
        assert (await bob_ws.receive_json())["type"] == "presence"
        await second.ping()
        await bob_ws.ping()
    finally:
        await first.close()
        await second.close()
        await bob_ws.close()


async def test_message_rate_limit(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("ws_message"))
    async with client_for(app) as http:
        alice = await register(http, "alice")
    ws, _ = await ws_login(app, alice)
    try:
        for _ in range(15):
            await ws.send_json({"type": "ping"})
        assert await ws.expect_closed() == 1008
    finally:
        await ws.close()


async def test_reconnecting_does_not_reset_the_message_limit(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("ws_message"))
    async with client_for(app) as http:
        alice = await register(http, "alice")
    first, _ = await ws_login(app, alice)
    second = None
    try:
        for _ in range(8):
            await first.ping()
        # A new socket for the same account does not start a fresh allowance.
        second, _ = await ws_login(app, alice)
        assert await first.expect_closed() == 4000
        for _ in range(8):
            await second.send_json({"type": "ping"})
        assert await second.expect_closed() == 1008  # signed in, so too many messages
    finally:
        await first.close()
        if second is not None:
            await second.close()


async def test_sign_ins_are_limited_per_account(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("ws_connect"))
    async with client_for(app) as http:
        alice = await register(http, "alice")
    sockets = []
    try:
        for i in range(10):  # each from its own address, so only the account counts
            ws, _ = await ws_login(app, alice, address=f"203.0.113.{i}")
            sockets.append(ws)
        async with WSClient(app, address="203.0.113.99") as eleventh:
            await eleventh.send_json({"type": "auth", "token": alice.access_token})
            assert await _close_of(eleventh) == (1008, "too many connections")
    finally:
        for ws in sockets:
            await ws.close()


async def _close_of(ws: WSClient) -> tuple[int, str]:
    with pytest.raises(WSClosed) as closed:
        await ws.receive_json()
    return closed.value.code, closed.value.reason


async def test_sockets_that_never_sign_in_are_limited_per_address(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("ws_connect"))
    async with client_for(app) as http:
        alice = await register(http, "alice")
    # Sockets that sign in give their address's place back...
    for _ in range(9):
        ws, _ = await ws_login(app, alice, address="198.51.100.5")
        await ws.close()
    # ...so the address still has all ten tries for sockets that never do.
    for _ in range(10):
        async with WSClient(app, address="198.51.100.5") as ws:
            await ws.send_json({"type": "auth", "token": "not a token"})
            assert await ws.expect_closed() == 4401
    async with WSClient(app, address="198.51.100.5") as ws:
        # Turned away before it can sign in, even with a good token.
        await ws.send_json({"type": "auth", "token": alice.access_token})
        assert await _close_of(ws) == (1008, "too many connections")
    async with WSClient(app, address="198.51.100.6") as ws:  # another address is unaffected
        await ws.send_json({"type": "auth", "token": alice.access_token})
        assert (await ws.receive_json())["type"] == "hello"


def _pause_after_the_first_token_check(
    monkeypatch: pytest.MonkeyPatch,
) -> tuple[asyncio.Event, asyncio.Event]:
    """Pause a sign-in between its token check and attaching to the hub, standing in for the
    check's session closing and the task being scheduled again."""
    original = ws_router._check_token
    checked, release = asyncio.Event(), asyncio.Event()

    async def check(token: object, services: object) -> object:
        claims = await original(token, services)  # type: ignore[arg-type]
        if not checked.is_set():
            checked.set()
            await release.wait()
        return claims

    monkeypatch.setattr(ws_router, "_check_token", check)
    return checked, release


async def test_socket_signing_in_during_a_password_change_is_closed(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    checked, release = _pause_after_the_first_token_check(monkeypatch)
    async with WSClient(app) as thief:
        await thief.send_json({"type": "auth", "token": alice.access_token})
        await checked.wait()
        changed = await client.post(
            "/v1/me/password",
            json={"current_password": alice.password, "new_password": "a brand new password"},
            headers=alice.headers,
        )
        assert changed.status_code == 204
        release.set()
        assert await thief.expect_closed() == 4401


async def test_socket_signing_in_during_account_deletion_is_closed(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    checked, release = _pause_after_the_first_token_check(monkeypatch)
    async with WSClient(app) as ws:
        await ws.send_json({"type": "auth", "token": alice.access_token})
        await checked.wait()
        deleted = await client.request(
            "DELETE", "/v1/me", json={"password": alice.password}, headers=alice.headers
        )
        assert deleted.status_code == 204
        release.set()
        assert await ws.expect_closed() == 4401


async def test_unchanged_presence_is_not_sent_again(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    bob_ws, _ = await ws_login(app, bob)
    alice_ws, _ = await ws_login(app, alice)
    try:
        assert (await bob_ws.receive_json())["type"] == "presence"  # Alice came online
        broadcasts = []
        original = ws_router.broadcast_presence

        async def counting_broadcast(*args: Any) -> None:
            broadcasts.append(args)
            await original(*args)

        monkeypatch.setattr(ws_router, "broadcast_presence", counting_broadcast)
        online = {"type": "presence", "status": "online", "activity": None}
        in_game = {"type": "presence", "status": "in_game", "activity": {"mode": "zm"}}
        the_same = {"type": "presence", "status": "in_game", "activity": {"mode": "zm", "joinable": False}}
        for message in (online, in_game, in_game, the_same, online):
            await alice_ws.send_json(message)
        await alice_ws.ping()
        seen = await bob_ws.messages_before_pong()
        assert [(m["status"], m["activity"]) for m in seen] == [
            ("in_game", {"mode": "zm", "joinable": False, "pack_code": None}),
            ("online", None),
        ]
        assert len(broadcasts) == 2  # the messages that changed nothing cost no queries
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_a_presence_fan_out_sends_the_newest_status(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    await befriend(client, alice, bob)
    bob_ws, _ = await ws_login(app, bob)
    first, _ = await ws_login(app, alice)
    second = None
    try:
        assert (await bob_ws.receive_json())["status"] == "online"
        entered, release = asyncio.Event(), asyncio.Event()
        original = presence_module.block_partner_ids

        async def paused(db: Any, user_id: Any) -> Any:
            result = await original(db, user_id)
            if user_id == alice.uuid and not entered.is_set():
                entered.set()
                await release.wait()  # the fan-out of "in_game" has its audience
            return result

        monkeypatch.setattr(presence_module, "block_partner_ids", paused)
        await first.send_json({"type": "presence", "status": "in_game"})
        await asyncio.wait_for(entered.wait(), 3)
        # Meanwhile Alice reconnects: her presence is "online" again, and Bob is told so.
        second, _ = await ws_login(app, alice)
        assert (await bob_ws.receive_json())["status"] == "online"
        release.set()
        await asyncio.sleep(0.1)  # the fan-out that waited finishes
        seen = await bob_ws.messages_before_pong()
        # It sends what is true when it sends, never the status it started with.
        assert [m["status"] for m in seen if m["type"] == "presence"] == ["online"]
    finally:
        await first.close()
        if second is not None:
            await second.close()
        await bob_ws.close()


async def test_socket_logs_carry_no_user_id(
    client: httpx.AsyncClient, app: FastAPI, caplog: pytest.LogCaptureFixture
) -> None:
    caplog.set_level(logging.INFO, logger="xerces_backend")
    alice = await register(client, "alice")
    ws, _ = await ws_login(app, alice)
    await ws.close()
    lines = [
        json.loads(JsonFormatter().format(record))
        for record in caplog.records
        if record.getMessage() in ("ws_connected", "ws_disconnected")
    ]
    # Presence 'nobody' or not, the log keeps no online history of anyone.
    assert [(line["event"], line["connections"]) for line in lines] == [
        ("ws_connected", 1),
        ("ws_disconnected", 0),
    ]
    assert alice.id not in json.dumps(lines)


async def test_binary_frames_rejected(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    ws, _ = await ws_login(app, alice)
    try:
        await ws.send_bytes(b"\x00\x01")
        assert await ws.expect_closed() == 1003
    finally:
        await ws.close()


async def test_password_change_closes_websocket(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    ws, _ = await ws_login(app, alice)
    try:
        response = await client.post(
            "/v1/me/password",
            json={"current_password": alice.password, "new_password": "a different password"},
            headers=alice.headers,
        )
        assert response.status_code == 204
        assert await ws.expect_closed() == 4401
    finally:
        await ws.close()


async def test_server_error_closes_with_1011(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    alice = await register(client, "alice")
    ws, _ = await ws_login(app, alice)

    async def broken(*_args: object, **_kwargs: object) -> None:
        raise RuntimeError("database went away")

    monkeypatch.setattr(ws_router, "broadcast_presence", broken)
    try:
        await ws.send_json({"type": "presence", "status": "away"})
        assert await ws.expect_closed() == 1011
    finally:
        await ws.close()


@pytest.mark.parametrize("token_source", ["refresh", "none"])
async def test_token_must_be_an_access_token(
    client: httpx.AsyncClient, app: FastAPI, token_source: str
) -> None:
    alice = await register(client, "alice")
    token = alice.refresh_token if token_source == "refresh" else ""
    async with WSClient(app) as ws:
        await ws.send_json({"type": "auth", "token": token})
        assert await ws.expect_closed() == 4401


async def test_lone_surrogate_token_is_just_invalid(app: FastAPI) -> None:
    # JSON can carry "\ud800", which is not valid Unicode; it must not crash the handler.
    async with WSClient(app) as ws:
        await ws.send_text('{"type": "auth", "token": "\\ud800"}')
        assert await ws.expect_closed() == 4401


async def test_socket_closes_when_its_token_expires_unless_renewed(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    alice_ws = await _connect_with(app, _token_expiring_soon(app, alice))
    bob_ws = await _connect_with(app, _token_expiring_soon(app, bob))
    try:
        # Bob's client sends a fresh access token over the socket; Alice's does not.
        await bob_ws.send_json({"type": "auth", "token": bob.access_token})
        assert await bob_ws.receive_json() == {"type": "auth_ok"}
        assert await alice_ws.expect_closed(timeout=5) == 4401
        await asyncio.sleep(0.2)
        await bob_ws.ping()  # still open after the token it opened with expired
    finally:
        await alice_ws.close()
        await bob_ws.close()


async def test_renewal_with_someone_elses_token_closes(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    bob = await register(client, "bob")
    ws, _ = await ws_login(app, alice)
    try:
        await ws.send_json({"type": "auth", "token": bob.access_token})
        assert await ws.expect_closed() == 4401
    finally:
        await ws.close()


async def test_logout_closes_websocket(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    ws, _ = await ws_login(app, alice)
    try:
        out = await client.post("/v1/auth/logout", json={"refresh_token": alice.refresh_token})
        assert out.status_code == 204
        assert await ws.expect_closed() == 4401
    finally:
        await ws.close()
    # Replaying the spent refresh token cannot keep kicking a newer socket.
    again, _ = await ws_login(app, alice)
    try:
        await client.post("/v1/auth/logout", json={"refresh_token": alice.refresh_token})
        await again.ping()
    finally:
        await again.close()


async def test_refresh_token_reuse_closes_websocket(client: httpx.AsyncClient, app: FastAPI) -> None:
    alice = await register(client, "alice")
    ws, _ = await ws_login(app, alice)
    try:
        rotated = await client.post("/v1/auth/refresh", json={"refresh_token": alice.refresh_token})
        assert rotated.status_code == 200
        await ws.ping()  # an ordinary refresh leaves the socket alone
        reuse = await client.post("/v1/auth/refresh", json={"refresh_token": alice.refresh_token})
        assert reuse.status_code == 401
        assert await ws.expect_closed() == 4401
    finally:
        await ws.close()


async def test_password_change_stops_older_tokens_reopening_the_socket(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    alice = await register(client, "alice")
    stolen = services_of(app).tokens.issue(alice.uuid)  # minted just before the change
    thief = await _connect_with(app, stolen)
    try:
        changed = await client.post(
            "/v1/me/password",
            json={"current_password": alice.password, "new_password": "a brand new password"},
            headers=alice.headers,
        )
        assert changed.status_code == 204
        assert await thief.expect_closed() == 4401
    finally:
        await thief.close()
    async with WSClient(app) as again:  # the copied token is still unexpired, but cut off
        await again.send_json({"type": "auth", "token": stolen})
        assert await again.expect_closed() == 4401
    login = await client.post(
        "/v1/auth/login", json={"username": "alice", "password": "a brand new password"}
    )
    fresh = await _connect_with(app, login.json()["access_token"])
    await fresh.close()
