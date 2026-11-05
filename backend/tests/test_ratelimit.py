"""Rate limits: the production rules, 429 with Retry-After, and the bucket maths."""

import asyncio
import logging
import os
import shutil
import subprocess
from collections import Counter
from collections.abc import Callable
from pathlib import Path
from typing import Any

import httpx
import pytest
from fastapi import FastAPI
from sqlalchemy import event
from sqlalchemy.exc import OperationalError
from sqlalchemy.ext.asyncio import AsyncSession
from uvicorn.middleware.proxy_headers import ProxyHeadersMiddleware

from app.ratelimit import DEFAULT_RULES, RateLimiter, Rule, TokenBucket
from app.security import client_address
from tests.helpers import (
    PASSWORD,
    AppFactory,
    client_for,
    hold_password_check,
    make_settings,
    register,
    services_of,
    strict_rules,
)

BACKEND = Path(__file__).resolve().parents[1]


def _slow_password_checks(app: FastAPI, monkeypatch: pytest.MonkeyPatch) -> None:
    """Make each password check take a while, as Argon2 does in production, so requests overlap."""
    passwords = services_of(app).passwords
    original = passwords.verify

    async def slow_verify(password_hash: str, password: str) -> bool:
        await asyncio.sleep(0.05)
        return await original(password_hash, password)

    monkeypatch.setattr(passwords, "verify", slow_verify)


async def test_register_limit_per_address(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("register"))
    async with client_for(app) as client:
        same = {"X-Forwarded-For": "203.0.113.7"}
        for i in range(3):
            ok = await client.post(
                "/v1/auth/register", json={"username": f"reg{i}", "password": PASSWORD}, headers=same
            )
            assert ok.status_code == 201
        limited = await client.post(
            "/v1/auth/register", json={"username": "reg3", "password": PASSWORD}, headers=same
        )
        assert limited.status_code == 429
        assert limited.json()["error"]["code"] == "rate_limited"
        assert int(limited.headers["retry-after"]) > 0
        # Another address is unaffected; so is the first entry being the one that counts.
        other = await client.post(
            "/v1/auth/register",
            json={"username": "reg4", "password": PASSWORD},
            headers={"X-Forwarded-For": "198.51.100.1, 203.0.113.7"},
        )
        assert other.status_code == 201


async def test_taken_usernames_count_against_registrations(app_factory: AppFactory) -> None:
    # Otherwise register would answer "does this account exist?" as often as anyone liked.
    app = await app_factory(rules=strict_rules("register"))
    async with client_for(app) as client:
        await register(client, "taken")
        probe = {"X-Forwarded-For": "203.0.113.8"}
        statuses = [
            (
                await client.post(
                    "/v1/auth/register", json={"username": "taken", "password": PASSWORD}, headers=probe
                )
            ).status_code
            for _ in range(5)
        ]
        assert statuses == [409, 409, 409, 429, 429]
        free = await client.post(
            "/v1/auth/register", json={"username": "free1", "password": PASSWORD}, headers=probe
        )
        assert free.status_code == 429


async def test_login_limit_per_address(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("login_address"))
    async with client_for(app) as client:
        headers = {"X-Forwarded-For": "192.0.2.10"}
        for i in range(10):
            response = await client.post(
                "/v1/auth/login",
                json={"username": f"someone{i}", "password": "wrong password"},
                headers=headers,
            )
            assert response.status_code == 401
        limited = await client.post(
            "/v1/auth/login", json={"username": "someone", "password": "x"}, headers=headers
        )
        assert limited.status_code == 429
        assert int(limited.headers["retry-after"]) >= 1


LOGIN_LIMITS = ("login_address", "login_username_address", "login_username")


def _count_password_checks(app: FastAPI, monkeypatch: pytest.MonkeyPatch) -> list[str]:
    """Count the passwords that reach the hash (each check is slow, as in `_slow_password_checks`)."""
    passwords = services_of(app).passwords
    original = passwords.verify
    checked: list[str] = []

    async def counting_verify(password_hash: str, password: str) -> bool:
        checked.append(password)
        await asyncio.sleep(0.05)
        return await original(password_hash, password)

    monkeypatch.setattr(passwords, "verify", counting_verify)
    return checked


async def _login(
    client: httpx.AsyncClient, username: str, password: str, address: str
) -> httpx.Response:
    return await client.post(
        "/v1/auth/login",
        json={"username": username, "password": password},
        headers={"X-Forwarded-For": address},
    )


async def test_login_limit_per_username(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules(*LOGIN_LIMITS))
    async with client_for(app) as client:
        user = await register(client, "target")
        # Ten addresses with five guesses each: fifty failures for one name.
        for address in range(10):
            for _ in range(5):
                response = await _login(client, "target", "wrong password", f"192.0.2.{address}")
                assert response.status_code == 401
        # From then on even the right password, from an address that never guessed, waits.
        limited = await _login(client, "TARGET", user.password, "192.0.2.99")
        assert limited.status_code == 429
        assert "retry-after" in limited.headers


async def test_one_address_cannot_lock_the_owner_out(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules(*LOGIN_LIMITS))
    async with client_for(app) as client:
        owner = await register(client, "target")
        for _ in range(5):
            assert (await _login(client, "target", "wrong password", "192.0.2.1")).status_code == 401
        # That address has had its five guesses at this name...
        limited = await _login(client, "target", "wrong password", "192.0.2.1")
        assert limited.status_code == 429
        assert int(limited.headers["retry-after"]) > 0
        # ...even with the right password (it cannot tell who is typing)...
        assert (await _login(client, "target", owner.password, "192.0.2.1")).status_code == 429
        # ...but the owner, from their own address, signs in as usual.
        assert (await _login(client, "TARGET", owner.password, "198.51.100.7")).status_code == 200


async def test_successful_logins_do_not_count_per_username(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("login_username_address", "login_username"))
    async with client_for(app) as client:
        user = await register(client, "regular")
        for _ in range(8):
            ok = await client.post(
                "/v1/auth/login", json={"username": "regular", "password": user.password}
            )
            assert ok.status_code == 200


async def test_parallel_guesses_from_one_address_get_five_checks(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(rules=strict_rules(*LOGIN_LIMITS))
    checked = _count_password_checks(app, monkeypatch)
    async with client_for(app) as client:
        victim = await register(client, "victim")
        results = await asyncio.gather(
            *(_login(client, "victim", f"wrong guess {i}", "198.51.100.1") for i in range(40))
        )
        # Only 5 guesses reach the password check, however many arrive at once.
        assert Counter(r.status_code for r in results) == {401: 5, 429: 35}
        assert len(checked) == 5
        # The owner is not locked out by it.
        assert (await _login(client, "victim", victim.password, "203.0.113.1")).status_code == 200


async def test_parallel_guesses_from_many_addresses_get_fifty_checks(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(rules=strict_rules(*LOGIN_LIMITS))
    checked = _count_password_checks(app, monkeypatch)
    async with client_for(app) as client:
        victim = await register(client, "victim")
        # Each from its own address, well inside that address's own limits.
        results = await asyncio.gather(
            *(_login(client, "victim", f"wrong guess {i}", f"2001:db8:{i:x}::1") for i in range(80))
        )
        # However many addresses, 50 guesses per 15 minutes reach the check for one name.
        assert Counter(r.status_code for r in results) == {401: 50, 429: 30}
        assert len(checked) == 50
        locked = await _login(client, "victim", victim.password, "203.0.113.1")
        assert locked.status_code == 429


async def test_server_errors_during_login_do_not_count(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(rules=strict_rules("login_username_address", "login_username"))
    transport = httpx.ASGITransport(app=app, raise_app_exceptions=False)
    async with httpx.AsyncClient(transport=transport, base_url="http://testserver") as client:
        owner = await register(client, "owner")
        original_scalar = AsyncSession.scalar
        outage = [True]

        async def flaky_scalar(self: AsyncSession, *args: Any, **kwargs: Any) -> Any:
            if outage[0]:
                raise OperationalError("SELECT", {}, ConnectionResetError("connection reset"))
            return await original_scalar(self, *args, **kwargs)

        monkeypatch.setattr(AsyncSession, "scalar", flaky_scalar)
        # A launcher keeps retrying its sign-in through a short database outage...
        for _ in range(8):
            login = await _login(client, "owner", owner.password, "192.0.2.50")
            assert login.status_code == 500
        outage[0] = False
        # ...and no password was checked, so none of it counted against the name.
        after = await _login(client, "owner", owner.password, "192.0.2.50")
        assert after.status_code == 200


async def test_a_full_hash_queue_answers_503_without_touching_the_database(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(
        rules=strict_rules("login_username_address"), hash_concurrency=1, hash_queue=2
    )
    services = services_of(app)
    async with client_for(app) as client:
        await register(client, "owner")
        original = services.passwords.verify
        release = asyncio.Event()

        async def held_verify(password_hash: str, password: str) -> bool:
            await release.wait()
            return await original(password_hash, password)

        monkeypatch.setattr(services.passwords, "verify", held_verify)
        lookups: list[str] = []

        def record(_conn: Any, _cursor: Any, statement: str, *_args: Any) -> None:
            if statement.lstrip().upper().startswith("SELECT") and "FROM users" in statement:
                lookups.append(statement)

        event.listen(services.engine.sync_engine, "before_cursor_execute", record)
        try:
            # One hashing and two waiting fill the line; the other two are turned away at once.
            burst = [
                asyncio.create_task(_login(client, "owner", f"guess {i}", "192.0.2.60"))
                for i in range(5)
            ]
            pending: set[asyncio.Task[httpx.Response]] = set(burst)
            while len(pending) > 3:
                done, pending = await asyncio.wait(
                    pending, timeout=5, return_when=asyncio.FIRST_COMPLETED
                )
                assert done, "nobody was turned away"
            release.set()
            await asyncio.gather(*burst)
        finally:
            event.remove(services.engine.sync_engine, "before_cursor_execute", record)
        responses = [task.result() for task in burst]
        assert Counter(r.status_code for r in responses) == {401: 3, 503: 2}
        turned_away = [r for r in responses if r.status_code == 503]
        assert all(r.headers["retry-after"] == "1" for r in turned_away)
        assert turned_away[0].json()["error"]["code"] == "busy"
        assert len(lookups) == 3  # the ones turned away never reached the database
        # Turned away is not a wrong password: of this address's 5 guesses, 2 are left.
        statuses = [
            (await _login(client, "owner", "guess", "192.0.2.60")).status_code for _ in range(3)
        ]
        assert statuses == [401, 401, 429]


async def test_requests_turned_away_busy_cost_their_address_nothing(
    app_factory: AppFactory,
) -> None:
    # Launchers retry a 503 after its Retry-After. Were each one counted, a flood of made-up
    # logins would also use up the logins and sign-ups of everyone retrying meanwhile.
    app = await app_factory(
        rules=strict_rules("login_address", "register"), hash_concurrency=1, hash_queue=0
    )
    async with client_for(app) as client:
        await register(client, "owner")
        started, release = hold_password_check(app, "held guess")
        held = asyncio.create_task(_login(client, "owner", "held guess", "192.0.2.70"))
        await started.wait()  # it fills the line
        here = "192.0.2.71"
        try:
            logins = [(await _login(client, "owner", PASSWORD, here)).status_code for _ in range(12)]
            sign_ups = [
                (
                    await client.post(
                        "/v1/auth/register",
                        json={"username": f"busy{i}", "password": PASSWORD},
                        headers={"X-Forwarded-For": here},
                    )
                ).status_code
                for i in range(4)
            ]
        finally:
            release.set()
            await held
        assert logins == [503] * 12
        assert sign_ups == [503] * 4
        # The address still has all 10 of its logins and all 3 of its sign-ups.
        assert (await _login(client, "owner", PASSWORD, here)).status_code == 200
        signed_up = await client.post(
            "/v1/auth/register",
            json={"username": "busy4", "password": PASSWORD},
            headers={"X-Forwarded-For": here},
        )
        assert signed_up.status_code == 201


async def test_parallel_guesses_cannot_pass_the_password_check_limit(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(rules=strict_rules("password_check"))
    _slow_password_checks(app, monkeypatch)
    async with client_for(app) as client:
        victim = await register(client, "victim")

        async def attempt(current_password: str) -> httpx.Response:
            # All a thief with the victim's access token has to send.
            return await client.post(
                "/v1/me/password",
                json={"current_password": current_password, "new_password": "the thief's now"},
                headers=victim.headers,
            )

        results = await asyncio.gather(*(attempt(f"dictionary word {i}") for i in range(12)))
        assert Counter(r.status_code for r in results) == {403: 5, 429: 7}
        assert (await attempt(victim.password)).status_code == 429
        unchanged = await client.post(
            "/v1/auth/login", json={"username": "victim", "password": victim.password}
        )
        assert unchanged.status_code == 200


async def test_server_errors_during_a_password_check_do_not_count(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(rules=strict_rules("password_check"))
    passwords = services_of(app).passwords
    transport = httpx.ASGITransport(app=app, raise_app_exceptions=False)
    async with httpx.AsyncClient(transport=transport, base_url="http://testserver") as client:
        user = await register(client, "owner")
        original = passwords.verify

        async def broken_verify(*_args: Any) -> bool:
            raise RuntimeError("the hashing thread failed")

        body = {"current_password": user.password, "new_password": "a brand new password"}
        monkeypatch.setattr(passwords, "verify", broken_verify)
        for _ in range(7):
            failed = await client.post("/v1/me/password", json=body, headers=user.headers)
            assert failed.status_code == 500
        monkeypatch.setattr(passwords, "verify", original)
        changed = await client.post("/v1/me/password", json=body, headers=user.headers)
        assert changed.status_code == 204


async def test_friend_request_daily_limit(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("friend_request"))
    async with client_for(app) as client:
        user = await register(client, "sender")
        for i in range(20):
            response = await client.post(
                "/v1/friends/requests", json={"username": f"nobody{i}"}, headers=user.headers
            )
            assert response.status_code == 202
        limited = await client.post(
            "/v1/friends/requests", json={"username": "nobody"}, headers=user.headers
        )
        assert limited.status_code == 429


def test_token_bucket_refills() -> None:
    now = [0.0]
    bucket = TokenBucket(Rule(2, 10), clock=lambda: now[0])
    assert bucket.take() and bucket.take()
    assert not bucket.take()
    now[0] = 5.0  # half the period refills one token
    assert bucket.take()
    assert not bucket.take()


def test_limiter_retry_after_and_prune() -> None:
    now = [0.0]
    limiter = RateLimiter({"x": Rule(1, 60)}, clock=lambda: now[0])
    assert limiter.hit("x", "k") == 0
    assert 59 < limiter.hit("x", "k") <= 60
    limiter.refund("x", "k")
    assert limiter.hit("x", "k") == 0
    assert len(limiter) == 1
    now[0] = 61.0
    assert limiter.prune() == 1
    assert len(limiter) == 0


def test_limiter_bounds_memory() -> None:
    limiter = RateLimiter({"x": Rule(5, 60)}, max_buckets=100)
    for i in range(1000):
        limiter.hit("x", str(i))
    assert len(limiter) <= 100


def test_flooding_new_keys_never_resets_an_exhausted_bucket() -> None:
    now = [0.0]
    limiter = RateLimiter(clock=lambda: now[0], max_buckets=1_000)
    for _ in range(50):  # fifty wrong passwords for the victim's name
        assert limiter.hit("login_username", "victim") == 0
    assert limiter.hit("login_username", "victim") > 0
    now[0] += 1.0
    # A flood of fresh keys for another rule (cheap: one registration attempt per address)
    # cannot touch this rule's buckets...
    for i in range(5_000):
        limiter.hit("register", f"address-{i}")
    # ...and fresh keys for the same rule are refused once it is full, rather than making room
    # by forgetting the bucket that is holding the name.
    refused = sum(limiter.hit("login_username", f"name-{i}") > 0 for i in range(5_000))
    assert refused == 5_000 - 999
    assert limiter.hit("login_username", "victim") > 0
    assert len(limiter) == 2_000


def test_full_buckets_make_room_and_scans_are_rationed() -> None:
    now = [0.0]
    limiter = RateLimiter({"x": Rule(1, 10)}, clock=lambda: now[0], max_buckets=10)
    scans = [0]
    drop_full = limiter._drop_full

    def counting_drop_full(*args: Any) -> int:
        scans[0] += 1
        return drop_full(*args)

    limiter._drop_full = counting_drop_full  # type: ignore[method-assign]
    for i in range(10):
        assert limiter.hit("x", f"spent-{i}") == 0
    # Every bucket still limits someone: new keys wait, and only one scan a second looks for room.
    for i in range(100):
        assert limiter.hit("x", f"new-{i}") > 0
    assert scans[0] == 1
    now[0] = 10.0  # all ten have refilled, so they are as good as new and can go
    assert limiter.hit("x", "new") == 0
    assert scans[0] == 2
    assert len(limiter) == 1


async def test_login_username_keys_are_bounded(client: httpx.AsyncClient, app: FastAPI) -> None:
    # 64 characters that NFKC turns into over 1,000; no account can have such a name.
    name = "\U0001f600" + "ﷺ" * 63
    response = await client.post("/v1/auth/login", json={"username": name, "password": "whatever"})
    assert response.status_code == 401
    await register(client, "named")
    await client.post("/v1/auth/login", json={"username": "NAMED", "password": "wrong password"})
    stores = services_of(app).limiter._stores
    assert list(stores["login_username"]) == ["named"]
    assert all(len(key) <= 24 + 1 + 32 for key in stores["login_username_address"])


async def test_replaying_a_spent_refresh_token_is_limited_before_any_work(
    app_factory: AppFactory, caplog: pytest.LogCaptureFixture
) -> None:
    app = await app_factory(rules=strict_rules("refresh"))
    engine = services_of(app).engine
    async with client_for(app) as client:
        mine = await register(client, "replayer")  # anyone can make such a token for themselves
        first = mine.refresh_token
        assert (await client.post("/v1/auth/refresh", json={"refresh_token": first})).status_code == 200
        writes: list[str] = []

        def record(_conn: Any, _cursor: Any, statement: str, *_args: Any) -> None:
            if statement.lstrip().upper().startswith("UPDATE"):
                writes.append(statement)

        caplog.set_level(logging.WARNING, logger="xerces_backend")
        event.listen(engine.sync_engine, "before_cursor_execute", record)
        try:
            responses = [
                await client.post(
                    "/v1/auth/refresh",
                    json={"refresh_token": first},
                    headers={"X-Forwarded-For": "203.0.113.9"},
                )
                for _ in range(100)
            ]
        finally:
            event.remove(engine.sync_engine, "before_cursor_execute", record)
    statuses = Counter(r.status_code for r in responses)
    assert set(statuses) == {401, 429} and statuses[429] >= 60  # 30 a minute, plus any refill
    # Only those inside the limit reached reuse detection (which locks the account and
    # revokes), and the theft warning went out once, when the family actually died.
    assert len([w for w in writes if "refresh_tokens" in w]) == statuses[401]
    warnings = [r for r in caplog.records if r.getMessage() == "refresh_token_reuse"]
    assert len(warnings) == 1


def test_default_rules_match_the_spec() -> None:
    def per(rule: Rule) -> tuple[float, float]:
        return rule.capacity, rule.period

    assert per(DEFAULT_RULES["register"]) == (3, 3600)
    assert per(DEFAULT_RULES["login_address"]) == (10, 900)
    assert per(DEFAULT_RULES["login_username_address"]) == (5, 900)
    assert per(DEFAULT_RULES["login_username"]) == (50, 900)
    assert per(DEFAULT_RULES["refresh"]) == (30, 60)
    assert per(DEFAULT_RULES["friend_request"]) == (20, 86400)
    assert per(DEFAULT_RULES["invite"]) == (30, 3600)
    assert per(DEFAULT_RULES["invite_recipient"]) == (1, 30)
    assert per(DEFAULT_RULES["report"]) == (10, 86400)
    assert per(DEFAULT_RULES["pack_create"]) == (20, 86400)
    assert per(DEFAULT_RULES["password_check"]) == (5, 900)
    assert per(DEFAULT_RULES["ws_message"]) == (10, 1)
    assert per(DEFAULT_RULES["ws_connect"]) == (10, 60)


async def test_client_addresses_are_hashed(app_factory: AppFactory) -> None:
    app = await app_factory()
    services = services_of(app)
    scope = {"type": "http", "headers": [(b"x-forwarded-for", b"203.0.113.5, 10.0.0.1")], "client": ("10.0.0.1", 1)}
    key = services.address_key(scope)
    assert "203.0.113.5" not in key and len(key) == 32
    # IPv6 addresses are grouped by /64.
    a = services.address_key({"headers": [(b"x-forwarded-for", b"2001:db8:1:2::1")]})
    b = services.address_key({"headers": [(b"x-forwarded-for", b"2001:db8:1:2:ffff::9")]})
    c = services.address_key({"headers": [(b"x-forwarded-for", b"2001:db8:1:3::1")]})
    assert a == b != c


async def test_without_trusted_proxy_the_socket_address_counts(client: httpx.AsyncClient) -> None:
    scope = {"headers": [(b"x-forwarded-for", b"1.2.3.4")], "client": ("10.9.8.7", 1234)}
    assert client_address(scope, trust_proxy=False) == "10.9.8.7"
    assert client_address(scope, trust_proxy=True) == "1.2.3.4"
    assert client_address({"headers": [(b"x-forwarded-for", b"[2001:db8::1]:443")]}, True) == "2001:db8::/64"
    assert client_address({"headers": [], "client": None}, True) == "unknown"


def test_which_forwarded_for_entry_counts() -> None:
    appended = {
        "headers": [(b"x-forwarded-for", b"203.0.113.9, 198.51.100.7")],
        "client": ("10.0.0.1", 1),
    }
    assert client_address(appended, True) == "203.0.113.9"  # default: the first entry
    assert client_address(appended, True, proxy_hops=1) == "198.51.100.7"  # what the edge added
    assert client_address(appended, True, proxy_hops=2) == "203.0.113.9"
    assert client_address(appended, True, proxy_hops=3) == "203.0.113.9"  # fewer entries than hops
    assert client_address(appended, False, proxy_hops=1) == "10.0.0.1"
    # A second header line holds further entries (uvicorn reads it the same way).
    two_lines = {
        "headers": [(b"x-forwarded-for", b"203.0.113.9"), (b"x-forwarded-for", b"198.51.100.7")]
    }
    assert client_address(two_lines, True, proxy_hops=1) == "198.51.100.7"
    blank = {"headers": [(b"x-forwarded-for", b" , ")], "client": ("10.0.0.1", 1)}
    assert client_address(blank, True) == "10.0.0.1"


async def _sign_ups_from_one_peer(app: Any, forwarded_for: Callable[[int], str]) -> list[int]:
    """Four sign-ups over one TCP peer, each claiming its own X-Forwarded-For."""
    transport = httpx.ASGITransport(app=app, client=("100.64.0.9", 40000))
    async with httpx.AsyncClient(transport=transport, base_url="http://testserver") as client:
        return [
            (
                await client.post(
                    "/v1/auth/register",
                    json={"username": f"spoof{i}", "password": PASSWORD},
                    headers={"X-Forwarded-For": forwarded_for(i)},
                )
            ).status_code
            for i in range(4)
        ]


async def test_forged_forwarded_for_is_ignored_without_trust(
    app_factory: AppFactory, tmp_path: Path
) -> None:
    # XERCES_TRUST_PROXY=0: start.sh also tells uvicorn to ignore the header, so the peer counts.
    settings = make_settings(tmp_path, XERCES_TRUST_PROXY=False)
    app = await app_factory(settings=settings, rules=strict_rules("register"))
    statuses = await _sign_ups_from_one_peer(app, lambda i: f"203.0.113.{i}")
    assert statuses == [201, 201, 201, 429]


async def test_appending_edge_with_proxy_hops(app_factory: AppFactory, tmp_path: Path) -> None:
    # An edge that appends: the client's forged entry comes first, the address it saw last.
    settings = make_settings(tmp_path, XERCES_PROXY_HOPS=1)
    app = await app_factory(settings=settings, rules=strict_rules("register"))
    edge = ProxyHeadersMiddleware(app, trusted_hosts="*")  # as start.sh runs uvicorn on Railway
    statuses = await _sign_ups_from_one_peer(edge, lambda i: f"203.0.113.{i}, 198.51.100.200")
    assert statuses == [201, 201, 201, 429]


def _posix_shell() -> str | None:
    shell = shutil.which("sh")
    if shell is None and (git := shutil.which("git")):  # Git for Windows ships one
        for parent in Path(git).resolve().parents:
            candidate = parent / "usr" / "bin" / "sh.exe"
            if candidate.is_file():
                return str(candidate)
    return shell


@pytest.mark.skipif(_posix_shell() is None, reason="needs a POSIX shell")
@pytest.mark.parametrize(
    ("value", "trusted"),
    [("1", True), ("TRUE", True), ("on", True), ("0", False), ("false", False), (None, False)],
)
def test_start_script_trusts_proxy_headers_only_when_told(
    tmp_path: Path, value: str | None, trusted: bool
) -> None:
    stubs = tmp_path / "bin"
    stubs.mkdir()
    for name, body in (("alembic", "exit 0"), ("uvicorn", 'printf "%s\\n" "$@"')):
        (stubs / name).write_text(f"#!/bin/sh\n{body}\n", newline="\n")
        (stubs / name).chmod(0o755)
    shell = _posix_shell()
    assert shell is not None
    env = {k: v for k, v in os.environ.items() if k.upper() not in {"XERCES_TRUST_PROXY", "PORT"}}
    env["PATH"] = os.pathsep.join([str(stubs), str(Path(shell).parent), env.get("PATH", "")])
    if value is not None:
        env["XERCES_TRUST_PROXY"] = value
    # The image strips carriage returns the same way (see Dockerfile).
    script = (BACKEND / "start.sh").read_text(encoding="utf-8").replace("\r\n", "\n")
    result = subprocess.run(
        [shell, "-s"], input=script, env=env, cwd=BACKEND, capture_output=True, text=True, check=True
    )
    args = result.stdout.split()
    assert args[:5] == ["app.main:app", "--host", "0.0.0.0", "--port", "8080"]
    if trusted:
        assert "--proxy-headers" in args and "--forwarded-allow-ips=*" in args
        assert "--no-proxy-headers" not in args
    else:
        assert "--no-proxy-headers" in args
        assert not any(arg.startswith("--forwarded-allow-ips") for arg in args)
