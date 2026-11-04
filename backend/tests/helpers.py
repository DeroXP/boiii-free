"""Test helpers: settings, user setup and an in-process WebSocket client."""

import asyncio
import base64
import itertools
import json
import uuid
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

import httpx
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi import FastAPI

from app.config import Settings, Tuning
from app.deps import Services
from app.models import Base
from app.ratelimit import DEFAULT_RULES, Rule

PASSWORD = "correct horse battery staple"
RELAXED_RULES = {name: Rule(1_000_000, 1) for name in DEFAULT_RULES}
_addresses = itertools.count(1)


class AppFactory(Protocol):
    """The `app_factory` fixture: builds an app with schema and lifespan, torn down after."""

    async def __call__(
        self,
        *,
        settings: Settings | None = None,
        rules: Mapping[str, Rule] | None = None,
        schema: bool = True,
        **tuning: Any,
    ) -> FastAPI: ...


def new_seed() -> str:
    key = Ed25519PrivateKey.generate()
    raw = key.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def sqlite_url(path: Path) -> str:
    return f"sqlite+aiosqlite:///{path.as_posix()}"


def make_settings(tmp_path: Path, **overrides: Any) -> Settings:
    values: dict[str, Any] = {
        "DATABASE_URL": sqlite_url(tmp_path / "test.db"),
        "XERCES_ENV": "test",
        "XERCES_TOKEN_PRIVATE_KEY": new_seed(),
        "XERCES_TOKEN_KEY_ID": "test-key",
        "XERCES_RATE_LIMIT_SECRET": "test-secret-" + "x" * 32,
        "XERCES_TRUST_PROXY": True,
        # Fast hashing for tests; production defaults are 2 / 19456 / 1.
        "XERCES_ARGON2_TIME_COST": 1,
        "XERCES_ARGON2_MEMORY_KIB": 8,
        "XERCES_ARGON2_PARALLELISM": 1,
    }
    values.update(overrides)
    return Settings(_env_file=None, **values)


def make_tuning(rules: Mapping[str, Rule] | None = None, **overrides: Any) -> Tuning:
    options: dict[str, Any] = {
        "rate_rules": RELAXED_RULES if rules is None else rules,
        "cleanup_interval": None,
        "ws_auth_timeout": 2.0,
        "configure_logging": False,
    }
    options.update(overrides)
    return Tuning(**options)


def strict_rules(*names: str) -> dict[str, Rule]:
    """Relaxed limits everywhere except the named rules, which keep production values."""
    rules = dict(RELAXED_RULES)
    for name in names:
        rules[name] = DEFAULT_RULES[name]
    return rules


def services_of(app: FastAPI) -> Services:
    return app.state.services


def client_for(app: FastAPI) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://testserver")


async def create_schema(app: FastAPI) -> None:
    async with services_of(app).engine.begin() as connection:
        await connection.run_sync(Base.metadata.create_all)


def next_address() -> str:
    n = next(_addresses)
    return f"10.{(n >> 16) & 255}.{(n >> 8) & 255}.{n & 255}"


@dataclass
class TestUser:
    __test__ = False  # tell pytest this is not a test class

    id: str
    username: str
    password: str
    access_token: str
    refresh_token: str
    data: dict[str, Any] = field(default_factory=dict)

    @property
    def uuid(self) -> uuid.UUID:
        return uuid.UUID(self.id)

    @property
    def headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.access_token}"}


async def register(
    client: httpx.AsyncClient,
    username: str,
    password: str = PASSWORD,
    *,
    email: str | None = None,
    address: str | None = None,
) -> TestUser:
    body: dict[str, Any] = {"username": username, "password": password}
    if email is not None:
        body["email"] = email
    response = await client.post(
        "/v1/auth/register", json=body, headers={"X-Forwarded-For": address or next_address()}
    )
    assert response.status_code == 201, response.text
    data = response.json()
    return TestUser(
        id=data["user"]["id"],
        username=data["user"]["username"],
        password=password,
        access_token=data["access_token"],
        refresh_token=data["refresh_token"],
        data=data,
    )


async def befriend(client: httpx.AsyncClient, a: TestUser, b: TestUser) -> None:
    sent = await client.post(
        "/v1/friends/requests", json={"username": b.username}, headers=a.headers
    )
    assert sent.status_code == 202, sent.text
    incoming = (await client.get("/v1/friends/requests", headers=b.headers)).json()["incoming"]
    request_id = next(r["id"] for r in incoming if r["user_id"] == a.id)
    accepted = await client.post(f"/v1/friends/requests/{request_id}/accept", headers=b.headers)
    assert accepted.status_code == 200, accepted.text


class WSClosed(Exception):
    def __init__(self, code: int, reason: str) -> None:
        super().__init__(f"closed with {code} {reason!r}")
        self.code = code
        self.reason = reason


class WSClient:
    """Speaks ASGI WebSocket to the app directly, in the test's own event loop."""

    def __init__(self, app: FastAPI, path: str = "/v1/ws") -> None:
        self.app = app
        self.path = path
        self._to_app: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._from_app: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._task: asyncio.Task[None] | None = None
        self.close_code: int | None = None

    async def connect(self) -> "WSClient":
        scope = {
            "type": "websocket",
            "asgi": {"version": "3.0", "spec_version": "2.4"},
            "http_version": "1.1",
            "scheme": "ws",
            "server": ("testserver", 80),
            "client": ("127.0.0.1", 50000),
            "root_path": "",
            "path": self.path,
            "raw_path": self.path.encode(),
            "query_string": b"",
            "headers": [(b"host", b"testserver")],
            "subprotocols": [],
            "extensions": {},
        }
        self._to_app.put_nowait({"type": "websocket.connect"})
        self._task = asyncio.create_task(self.app(scope, self._to_app.get, self._on_send))
        message = await asyncio.wait_for(self._from_app.get(), 5)
        assert message["type"] == "websocket.accept", message
        return self

    async def _on_send(self, message: dict[str, Any]) -> None:
        if message["type"] == "websocket.close":
            self.close_code = message.get("code", 1000)
            # A real client answers the close handshake; the server then sees a disconnect.
            self._to_app.put_nowait({"type": "websocket.disconnect", "code": self.close_code})
        await self._from_app.put(message)

    async def send_json(self, data: Any) -> None:
        await self.send_text(json.dumps(data))

    async def send_text(self, text: str) -> None:
        self._to_app.put_nowait({"type": "websocket.receive", "text": text})

    async def send_bytes(self, data: bytes) -> None:
        self._to_app.put_nowait({"type": "websocket.receive", "bytes": data})

    async def receive_json(self, timeout: float = 3.0) -> dict[str, Any]:
        message = await asyncio.wait_for(self._from_app.get(), timeout)
        if message["type"] == "websocket.close":
            raise WSClosed(message.get("code", 1000), message.get("reason") or "")
        assert message["type"] == "websocket.send", message
        return json.loads(message["text"])

    async def receive_until(self, kind: str, timeout: float = 3.0) -> dict[str, Any]:
        """Skip messages until one of type `kind` arrives."""
        while True:
            message = await self.receive_json(timeout)
            if message.get("type") == kind:
                return message

    async def expect_closed(self, timeout: float = 3.0) -> int:
        while True:
            message = await asyncio.wait_for(self._from_app.get(), timeout)
            if message["type"] == "websocket.close":
                return int(message.get("code", 1000))

    async def ping(self) -> None:
        """Round trip: everything queued for this client before the pong arrives first."""
        await self.send_json({"type": "ping"})
        message = await self.receive_json()
        assert message == {"type": "pong"}, message

    async def messages_before_pong(self) -> list[dict[str, Any]]:
        """Send a ping; return everything that reached this client before the pong."""
        await self.send_json({"type": "ping"})
        seen = []
        while (message := await self.receive_json()) != {"type": "pong"}:
            seen.append(message)
        return seen

    async def close(self) -> None:
        if self._task is None:
            return
        if not self._task.done():
            if self.close_code is None:
                self._to_app.put_nowait({"type": "websocket.disconnect", "code": 1000})
            await asyncio.wait_for(self._task, 5)
        self._task.result()

    async def __aenter__(self) -> "WSClient":
        return await self.connect()

    async def __aexit__(self, *exc_info: object) -> None:
        await self.close()


async def ws_login(app: FastAPI, user: TestUser) -> tuple[WSClient, dict[str, Any]]:
    """Open /v1/ws as `user`; returns the client and its hello message."""
    ws = await WSClient(app).connect()
    await ws.send_json({"type": "auth", "token": user.access_token})
    hello = await ws.receive_json()
    assert hello["type"] == "hello", hello
    return ws, hello
