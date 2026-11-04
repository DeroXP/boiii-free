"""Rate limits: the production rules, 429 with Retry-After, and the bucket maths."""

import asyncio
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
from uvicorn.middleware.proxy_headers import ProxyHeadersMiddleware

from app.ratelimit import DEFAULT_RULES, RateLimiter, Rule, TokenBucket
from app.security import client_address
from tests.helpers import (
    PASSWORD,
    AppFactory,
    client_for,
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


async def test_login_limit_per_username(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("login_username"))
    async with client_for(app) as client:
        user = await register(client, "target")
        for i in range(5):
            response = await client.post(
                "/v1/auth/login",
                json={"username": "target", "password": "wrong password"},
                headers={"X-Forwarded-For": f"192.0.2.{i + 1}"},
            )
            assert response.status_code == 401
        # Even from a new address and with the right password, the name is locked for a while.
        limited = await client.post(
            "/v1/auth/login",
            json={"username": "TARGET", "password": user.password},
            headers={"X-Forwarded-For": "192.0.2.99"},
        )
        assert limited.status_code == 429
        assert "retry-after" in limited.headers


async def test_successful_logins_do_not_count_per_username(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("login_username"))
    async with client_for(app) as client:
        user = await register(client, "regular")
        for _ in range(8):
            ok = await client.post(
                "/v1/auth/login", json={"username": "regular", "password": user.password}
            )
            assert ok.status_code == 200


async def test_parallel_guesses_cannot_pass_the_username_limit(
    app_factory: AppFactory, monkeypatch: pytest.MonkeyPatch
) -> None:
    app = await app_factory(rules=strict_rules("login_address", "login_username"))
    _slow_password_checks(app, monkeypatch)
    async with client_for(app) as client:
        victim = await register(client, "victim")

        async def guess(i: int) -> httpx.Response:
            # Each from its own address, well inside that address's 10 per 15 minutes.
            return await client.post(
                "/v1/auth/login",
                json={"username": "victim", "password": f"wrong guess {i}"},
                headers={"X-Forwarded-For": f"198.51.100.{i + 1}"},
            )

        results = await asyncio.gather(*(guess(i) for i in range(40)))
        # Only 5 guesses reach the password check, however many arrive at once.
        assert Counter(r.status_code for r in results) == {401: 5, 429: 35}
        right = await client.post(
            "/v1/auth/login",
            json={"username": "victim", "password": victim.password},
            headers={"X-Forwarded-For": "198.51.100.200"},
        )
        assert right.status_code == 429


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


def test_default_rules_match_the_spec() -> None:
    def per(rule: Rule) -> tuple[float, float]:
        return rule.capacity, rule.period

    assert per(DEFAULT_RULES["register"]) == (3, 3600)
    assert per(DEFAULT_RULES["login_address"]) == (10, 900)
    assert per(DEFAULT_RULES["login_username"]) == (5, 900)
    assert per(DEFAULT_RULES["refresh"]) == (30, 60)
    assert per(DEFAULT_RULES["friend_request"]) == (20, 86400)
    assert per(DEFAULT_RULES["invite"]) == (30, 3600)
    assert per(DEFAULT_RULES["invite_recipient"]) == (1, 30)
    assert per(DEFAULT_RULES["report"]) == (10, 86400)
    assert per(DEFAULT_RULES["pack_create"]) == (20, 86400)
    assert per(DEFAULT_RULES["ws_message"]) == (10, 1)


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
