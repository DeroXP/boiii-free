"""Register, login, access tokens and refresh-token rotation."""

import asyncio
import time
import uuid
from datetime import timedelta

import httpx
import jwt
import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi import FastAPI
from sqlalchemy import select, update

from app.models import RefreshToken, User, utcnow
from app.passwords import PasswordService
from app.routers import auth as auth_router
from tests.helpers import (
    AppFactory,
    TestUser,
    client_for,
    hold_password_check,
    make_settings,
    register,
    services_of,
    ws_login,
)

NEW_PASSWORD = "the owner's new password"


async def test_register_returns_tokens_and_user(client: httpx.AsyncClient) -> None:
    response = await client.post(
        "/v1/auth/register",
        json={"username": "Alice", "password": "a long password", "email": "Alice@Example.com"},
    )
    assert response.status_code == 201
    body = response.json()
    assert body["token_type"] == "Bearer"
    assert body["expires_in"] == 600
    assert body["user"]["username"] == "Alice"
    assert body["user"]["email"] == "alice@example.com"
    assert body["user"]["privacy"] == {
        "friend_requests": "everyone",
        "presence": "friends",
        "invites": "friends",
    }
    assert "password_hash" not in body["user"]
    me = await client.get("/v1/me", headers={"Authorization": f"Bearer {body['access_token']}"})
    assert me.status_code == 200
    assert me.json()["id"] == body["user"]["id"]


async def test_username_is_unique_case_insensitively(client: httpx.AsyncClient) -> None:
    await register(client, "Alice")
    response = await client.post(
        "/v1/auth/register", json={"username": "aLiCe", "password": "another password"}
    )
    assert response.status_code == 409
    assert response.json()["error"]["code"] == "username_taken"


async def test_login_and_wrong_password(client: httpx.AsyncClient) -> None:
    user = await register(client, "bob")
    ok = await client.post("/v1/auth/login", json={"username": "BOB", "password": user.password})
    assert ok.status_code == 200
    assert ok.json()["user"]["id"] == user.id
    assert ok.json()["refresh_token"] != user.refresh_token

    wrong = await client.post("/v1/auth/login", json={"username": "bob", "password": "nope nope nope"})
    unknown = await client.post(
        "/v1/auth/login", json={"username": "nobody-here", "password": "nope nope nope"}
    )
    for response in (wrong, unknown):
        assert response.status_code == 401
        assert response.json() == {
            "error": {"code": "invalid_credentials", "message": "Invalid username or password."}
        }


async def test_unknown_username_still_burns_a_hash(app: FastAPI, monkeypatch) -> None:
    burned = []
    passwords = services_of(app).passwords
    original = passwords.verify

    async def counting_verify(password_hash: str, password: str) -> bool:
        burned.append(password_hash)
        return await original(password_hash, password)

    monkeypatch.setattr(passwords, "verify", counting_verify)
    async with client_for(app) as client:
        response = await client.post(
            "/v1/auth/login", json={"username": "ghost", "password": "whatever you say"}
        )
    assert response.status_code == 401
    assert len(burned) == 1


async def test_refresh_rotates(client: httpx.AsyncClient) -> None:
    user = await register(client, "carol")
    first = await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
    assert first.status_code == 200
    pair = first.json()
    assert pair["refresh_token"] != user.refresh_token
    assert pair["token_type"] == "Bearer"
    me = await client.get("/v1/me", headers={"Authorization": f"Bearer {pair['access_token']}"})
    assert me.status_code == 200

    second = await client.post("/v1/auth/refresh", json={"refresh_token": pair["refresh_token"]})
    assert second.status_code == 200


async def test_refresh_reuse_revokes_the_family(client: httpx.AsyncClient, app: FastAPI) -> None:
    user = await register(client, "dave")
    rotated = (
        await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
    ).json()["refresh_token"]

    # The old token comes back (stolen copy): rejected, and the whole family dies.
    reuse = await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
    assert reuse.status_code == 401
    assert reuse.json()["error"]["code"] == "invalid_token"
    latest = await client.post("/v1/auth/refresh", json={"refresh_token": rotated})
    assert latest.status_code == 401

    async with services_of(app).sessionmaker() as db:
        rows = (
            await db.scalars(select(RefreshToken).where(RefreshToken.user_id == user.uuid))
        ).all()
    assert rows and all(row.revoked_at is not None for row in rows)

    # Other sessions (other families) are untouched.
    other = (
        await client.post("/v1/auth/login", json={"username": "dave", "password": user.password})
    ).json()
    fresh = await client.post("/v1/auth/refresh", json={"refresh_token": other["refresh_token"]})
    assert fresh.status_code == 200


async def test_garbage_refresh_token(client: httpx.AsyncClient) -> None:
    for token in ("x", "a" * 43, "not base64 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"):
        response = await client.post("/v1/auth/refresh", json={"refresh_token": token})
        assert response.status_code == 401


async def test_logout_revokes_the_family(client: httpx.AsyncClient) -> None:
    user = await register(client, "erin")
    rotated = (
        await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
    ).json()["refresh_token"]
    out = await client.post("/v1/auth/logout", json={"refresh_token": rotated})
    assert out.status_code == 204
    again = await client.post("/v1/auth/refresh", json={"refresh_token": rotated})
    assert again.status_code == 401
    # Logging out with an unknown token is still a 204.
    unknown = await client.post("/v1/auth/logout", json={"refresh_token": "b" * 43})
    assert unknown.status_code == 204


async def test_password_change_revokes_every_session(client: httpx.AsyncClient) -> None:
    user = await register(client, "frank")
    second = (
        await client.post("/v1/auth/login", json={"username": "frank", "password": user.password})
    ).json()

    wrong = await client.post(
        "/v1/me/password",
        json={"current_password": "not my password", "new_password": "brand new password"},
        headers=user.headers,
    )
    assert wrong.status_code == 403

    changed = await client.post(
        "/v1/me/password",
        json={"current_password": user.password, "new_password": "brand new password"},
        headers=user.headers,
    )
    assert changed.status_code == 204
    for token in (user.refresh_token, second["refresh_token"]):
        response = await client.post("/v1/auth/refresh", json={"refresh_token": token})
        assert response.status_code == 401

    old = await client.post("/v1/auth/login", json={"username": "frank", "password": user.password})
    assert old.status_code == 401
    new = await client.post(
        "/v1/auth/login", json={"username": "frank", "password": "brand new password"}
    )
    assert new.status_code == 200


async def test_password_change_cuts_off_older_access_tokens(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    user = await register(client, "grace")
    tokens = services_of(app).tokens
    # Copies of access tokens with minutes left: one from a few seconds before the change, and
    # one from the same second (a thief refreshing in a loop has one like that).
    stolen = [tokens.issue(user.uuid, now=time.time() - 5), tokens.issue(user.uuid)]
    for token in stolen:
        assert (await client.get("/v1/me", headers={"Authorization": f"Bearer {token}"})).status_code == 200

    changed = await client.post(
        "/v1/me/password",
        json={"current_password": user.password, "new_password": "brand new password"},
        headers=user.headers,
    )
    assert changed.status_code == 204
    for token in stolen:
        rejected = await client.get("/v1/me", headers={"Authorization": f"Bearer {token}"})
        assert rejected.status_code == 401
        assert rejected.json()["error"]["code"] == "invalid_token"

    # Signing in again right after the change works at once.
    login = await client.post(
        "/v1/auth/login", json={"username": "grace", "password": "brand new password"}
    )
    fresh = {"Authorization": f"Bearer {login.json()['access_token']}"}
    assert (await client.get("/v1/me", headers=fresh)).status_code == 200
    export = (await client.get("/v1/me/export", headers=fresh)).json()
    assert export["user"]["tokens_valid_after"] is not None


async def test_new_password_must_differ_from_username(client: httpx.AsyncClient) -> None:
    user = await register(client, "Gertrude12")
    response = await client.post(
        "/v1/me/password",
        json={"current_password": user.password, "new_password": "gertrude12"},
        headers=user.headers,
    )
    assert response.status_code == 422


def _forge(
    app: FastAPI, *, key: Ed25519PrivateKey | None = None, **overrides: object
) -> str:
    services = services_of(app)
    now = int(time.time())
    claims: dict[str, object] = {
        "iss": "xerces",
        "aud": "xerces-client",
        "typ": "access",
        "sub": str(uuid.uuid4()),
        "iat": now,
        "exp": now + 600,
        "jti": "x",
    }
    claims.update(overrides)
    return jwt.encode(
        claims,
        key or services.config.signing_key,
        algorithm="EdDSA",
        headers={"kid": services.config.key_id},
    )


@pytest.mark.parametrize(
    ("overrides", "foreign_key"),
    [
        ({"aud": "someone-else"}, False),
        ({"iss": "not-xerces"}, False),
        ({"typ": "refresh"}, False),
        ({"exp": int(time.time()) - 5}, False),
        ({}, True),
    ],
    ids=["audience", "issuer", "type", "expired", "foreign-key"],
)
async def test_access_token_checks(
    app: FastAPI, overrides: dict[str, object], foreign_key: bool
) -> None:
    async with client_for(app) as client:
        user = await register(client, "henry")
        valid = _forge(app, sub=user.id)
        assert (await client.get("/v1/me", headers={"Authorization": f"Bearer {valid}"})).status_code == 200
        key = Ed25519PrivateKey.generate() if foreign_key else None
        token = _forge(app, key=key, sub=user.id, **overrides)
        response = await client.get("/v1/me", headers={"Authorization": f"Bearer {token}"})
    assert response.status_code == 401
    assert response.json()["error"]["code"] == "invalid_token"


async def test_access_token_rejects_other_algorithms(app: FastAPI) -> None:
    services = services_of(app)
    async with client_for(app) as client:
        user = await register(client, "ivy")
        hs256 = jwt.encode(
            {
                "iss": "xerces",
                "aud": "xerces-client",
                "typ": "access",
                "sub": user.id,
                "iat": int(time.time()),
                "exp": int(time.time()) + 600,
                "jti": "x",
            },
            "a shared secret that is long enough for hs256",
            algorithm="HS256",
            headers={"kid": services.config.key_id},
        )
        response = await client.get("/v1/me", headers={"Authorization": f"Bearer {hs256}"})
        assert response.status_code == 401
        good = await client.get("/v1/me", headers=user.headers)
        assert good.status_code == 200
        assert jwt.get_unverified_header(user.access_token) == {
            "alg": "EdDSA",
            "kid": "test-key",
            "typ": "JWT",
        }


async def test_rehash_on_login_when_parameters_change(app_factory: AppFactory, tmp_path) -> None:
    weak = make_settings(tmp_path)
    app = await app_factory(settings=weak)
    async with client_for(app) as client:
        user = await register(client, "julia")
    async with services_of(app).sessionmaker() as db:
        before = await db.scalar(select(User.password_hash).where(User.username_lower == "julia"))

    stronger = make_settings(tmp_path, XERCES_ARGON2_TIME_COST=2, XERCES_ARGON2_MEMORY_KIB=64)
    app2 = await app_factory(settings=stronger, schema=False)
    async with client_for(app2) as client:
        response = await client.post(
            "/v1/auth/login", json={"username": "julia", "password": user.password}
        )
        assert response.status_code == 200
    async with services_of(app2).sessionmaker() as db:
        after = await db.scalar(select(User.password_hash).where(User.username_lower == "julia"))
    assert before is not None and after is not None
    assert before != after
    assert "m=64,t=2,p=1" in after
    assert not services_of(app2).passwords.needs_rehash(after)


async def test_password_service_needs_rehash() -> None:
    weak = PasswordService(time_cost=1, memory_kib=8, parallelism=1)
    strong = PasswordService(time_cost=2, memory_kib=16, parallelism=1)
    password_hash = await weak.hash("some password here")
    assert password_hash.startswith("$argon2id$")
    assert await strong.verify(password_hash, "some password here")
    assert strong.needs_rehash(password_hash)
    assert not weak.needs_rehash(password_hash)
    assert not await weak.verify("not a hash", "some password here")


async def _live_tokens(app: FastAPI, user: TestUser) -> list[RefreshToken]:
    async with services_of(app).sessionmaker() as db:
        rows = await db.scalars(
            select(RefreshToken).where(
                RefreshToken.user_id == user.uuid, RefreshToken.revoked_at.is_(None)
            )
        )
        return list(rows.all())


async def _change_password(
    client: httpx.AsyncClient, user: TestUser, new_password: str = NEW_PASSWORD
) -> httpx.Response:
    return await client.post(
        "/v1/me/password",
        json={"current_password": user.password, "new_password": new_password},
        headers=user.headers,
    )


async def _login(client: httpx.AsyncClient, username: str, password: str) -> httpx.Response:
    return await client.post("/v1/auth/login", json={"username": username, "password": password})


async def test_login_racing_a_password_change_gets_no_session(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    owner = await register(client, "owner")
    started, release = hold_password_check(app, owner.password)
    # Someone who knows the old password signs in, and their hash takes a while...
    thief = asyncio.create_task(_login(client, "owner", owner.password))
    await started.wait()
    # ...while the owner changes the password to lock them out.
    assert (await _change_password(client, owner)).status_code == 204
    release.set()
    login = await thief
    assert login.status_code == 401
    assert login.json()["error"]["code"] == "invalid_credentials"
    assert await _live_tokens(app, owner) == []


async def test_login_racing_a_password_change_cannot_bring_the_old_password_back(
    app_factory: AppFactory, tmp_path
) -> None:
    # Created under weaker Argon2 settings, so the next login with it rehashes the password.
    weak = await app_factory(settings=make_settings(tmp_path))
    async with client_for(weak) as client:
        owner = await register(client, "owner")
    stronger = make_settings(tmp_path, XERCES_ARGON2_TIME_COST=2, XERCES_ARGON2_MEMORY_KIB=64)
    app = await app_factory(settings=stronger, schema=False)
    async with client_for(app) as client:
        refreshed = await client.post("/v1/auth/refresh", json={"refresh_token": owner.refresh_token})
        owner.access_token = refreshed.json()["access_token"]  # tokens of the old key do not work
        started, release = hold_password_check(app, owner.password)
        thief = asyncio.create_task(_login(client, "owner", owner.password))
        await started.wait()
        assert (await _change_password(client, owner)).status_code == 204
        release.set()
        assert (await thief).status_code == 401
        # The rehash of the old password never landed on top of the new one.
        assert (await _login(client, "owner", NEW_PASSWORD)).status_code == 200
        assert (await _login(client, "owner", owner.password)).status_code == 401


async def test_password_changes_racing_each_other_cannot_both_win(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    owner = await register(client, "owner")
    started, release = hold_password_check(app, owner.password)
    # A thief with a copy of the access token and the old password changes it too, slowly.
    thief = asyncio.create_task(_change_password(client, owner, "the thief's password"))
    await started.wait()
    assert (await _change_password(client, owner)).status_code == 204
    release.set()
    answer = await thief
    assert answer.status_code == 403
    assert answer.json()["error"]["code"] == "invalid_password"
    assert (await _login(client, "owner", NEW_PASSWORD)).status_code == 200
    assert (await _login(client, "owner", "the thief's password")).status_code == 401


async def test_deletion_racing_a_password_change_keeps_the_account(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    owner = await register(client, "owner")
    started, release = hold_password_check(app, owner.password)
    deleting = asyncio.create_task(
        client.request("DELETE", "/v1/me", json={"password": owner.password}, headers=owner.headers)
    )
    await started.wait()
    assert (await _change_password(client, owner)).status_code == 204
    release.set()
    assert (await deleting).status_code == 403
    assert (await _login(client, "owner", NEW_PASSWORD)).status_code == 200


async def test_login_racing_account_deletion_answers_401(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    owner = await register(client, "owner")
    started, release = hold_password_check(app, owner.password)
    login = asyncio.create_task(_login(client, "owner", owner.password))
    await started.wait()
    deleted = await client.request(
        "DELETE", "/v1/me", json={"password": owner.password}, headers=owner.headers
    )
    assert deleted.status_code == 204
    release.set()
    response = await login
    assert response.status_code == 401
    assert response.json()["error"]["code"] == "invalid_credentials"


def _pause_after_rotating(monkeypatch: pytest.MonkeyPatch) -> asyncio.Event:
    """Rotate as usual, then wait before the commit, as latency to the database would.

    On PostgreSQL a revocation running in that gap used to miss the new token (a single UPDATE
    does not see a row inserted after it began). On SQLite the database's one writer at a time
    serializes them anyway, so there these pass either way.
    """
    original = auth_router.rotate_refresh_token
    rotated = asyncio.Event()

    async def rotate(*args: object) -> str | None:
        new_token = await original(*args)  # type: ignore[arg-type]
        rotated.set()
        await asyncio.sleep(0.3)
        return new_token

    monkeypatch.setattr(auth_router, "rotate_refresh_token", rotate)
    return rotated


async def test_password_change_revokes_a_refresh_in_flight(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    owner = await register(client, "owner")
    rotated = _pause_after_rotating(monkeypatch)
    refreshing = asyncio.create_task(
        client.post("/v1/auth/refresh", json={"refresh_token": owner.refresh_token})
    )
    await rotated.wait()
    assert (await _change_password(client, owner)).status_code == 204
    refreshed = await refreshing
    assert await _live_tokens(app, owner) == []
    monkeypatch.undo()
    again = await client.post(
        "/v1/auth/refresh", json={"refresh_token": refreshed.json()["refresh_token"]}
    )
    assert again.status_code == 401


async def test_logout_revokes_a_refresh_of_its_own_session_in_flight(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    owner = await register(client, "owner")
    socket, _ = await ws_login(app, owner)
    try:
        rotated = _pause_after_rotating(monkeypatch)
        refreshing = asyncio.create_task(
            client.post("/v1/auth/refresh", json={"refresh_token": owner.refresh_token})
        )
        await rotated.wait()
        out = await client.post("/v1/auth/logout", json={"refresh_token": owner.refresh_token})
        assert out.status_code == 204
        await refreshing
        assert await _live_tokens(app, owner) == []
        assert await socket.expect_closed() == 4401
    finally:
        await socket.close()


async def test_reuse_detection_revokes_a_rotation_in_flight(
    client: httpx.AsyncClient, app: FastAPI, monkeypatch: pytest.MonkeyPatch
) -> None:
    owner = await register(client, "owner")
    r1 = owner.refresh_token
    # A thief with a copy of R1 got there first, and keeps refreshing.
    r2 = (await client.post("/v1/auth/refresh", json={"refresh_token": r1})).json()["refresh_token"]
    rotated = _pause_after_rotating(monkeypatch)
    thief_next = asyncio.create_task(client.post("/v1/auth/refresh", json={"refresh_token": r2}))
    await rotated.wait()
    owner_attempt = await client.post("/v1/auth/refresh", json={"refresh_token": r1})
    assert owner_attempt.status_code == 401  # reuse detected...
    r3 = (await thief_next).json()["refresh_token"]
    monkeypatch.undo()
    # ...and the family dies with the token that was being added to it at that moment.
    assert (await client.post("/v1/auth/refresh", json={"refresh_token": r3})).status_code == 401
    assert await _live_tokens(app, owner) == []


async def test_refresh_token_older_than_a_password_change_is_refused(
    client: httpx.AsyncClient, app: FastAPI
) -> None:
    user = await register(client, "olduser")
    async with services_of(app).sessionmaker() as db:
        # As if a password change had somehow missed this token: the cut-off is past it.
        await db.execute(
            update(User)
            .where(User.id == user.uuid)
            .values(tokens_valid_after=utcnow() + timedelta(seconds=5))
        )
        await db.commit()
    response = await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
    assert response.status_code == 401
    assert await _live_tokens(app, user) == []
