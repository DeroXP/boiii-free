"""Logs are JSON lines and never carry tokens, passwords, emails or client addresses."""

import json
import logging
import subprocess
import sys
from pathlib import Path

import httpx
import pytest

from app.log import JsonFormatter, NoClientAddressFilter
from tests.helpers import register

BACKEND = Path(__file__).resolve().parents[1]


async def test_logs_hold_no_secrets(client: httpx.AsyncClient, caplog: pytest.LogCaptureFixture) -> None:
    caplog.set_level(logging.DEBUG)
    address = "203.0.113.99"
    user = await register(client, "loggy", email="loggy@example.com", address=address)
    headers = {"X-Forwarded-For": address}
    await client.post("/v1/auth/login", json={"username": "loggy", "password": user.password}, headers=headers)
    await client.post(
        "/v1/auth/login", json={"username": "loggy", "password": "wrong password!"}, headers=headers
    )
    rotated = await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
    await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})  # reuse
    await client.get("/v1/me", headers=user.headers)

    formatter = JsonFormatter()
    # The service's own events, plus anything any library logs at WARNING or above (what
    # production keeps: see app.log.LOGGING_CONFIG).
    records = [
        r for r in caplog.records if r.name.startswith("xerces_backend") or r.levelno >= logging.WARNING
    ]
    lines = [formatter.format(record) for record in records]
    text = "\n".join(lines)
    for secret in (
        user.password,
        "wrong password!",
        user.refresh_token,
        rotated.json()["refresh_token"],
        user.access_token,
        "loggy@example.com",
        address,
        "loggy",
    ):
        assert secret not in text
    events = {json.loads(line)["event"] for line in lines}
    assert {"user_registered", "login", "login_failed", "refresh_token_reuse", "request"} <= events
    request = next(json.loads(line) for line in lines if json.loads(line)["event"] == "request")
    assert set(request) == {"ts", "level", "logger", "event", "method", "route", "status", "ms"}


def test_uvicorn_client_address_lines_are_dropped() -> None:
    keep = logging.LogRecord("uvicorn.error", logging.INFO, "", 0, "Application startup complete.", (), None)
    drop = logging.LogRecord(
        "uvicorn.error", logging.INFO, "", 0, '%s - "WebSocket %s" [accepted]', ("1.2.3.4:5", "/v1/ws"), None
    )
    assert NoClientAddressFilter().filter(keep)
    assert not NoClientAddressFilter().filter(drop)


def test_configure_logging_writes_json_lines() -> None:
    code = (
        "import logging\n"
        "from app.log import configure_logging, log_event\n"
        "configure_logging()\n"
        "log_event('hello', answer=42)\n"
        "logging.getLogger('uvicorn.error').info('%s - \"WebSocket %s\" [accepted]', '1.2.3.4:5', '/v1/ws')\n"
        "logging.getLogger('uvicorn.error').info('Application startup complete.')\n"
        "logging.getLogger('uvicorn.access').info('1.2.3.4 - GET / 200')\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", code], cwd=BACKEND, capture_output=True, text=True, check=True
    )
    lines = [json.loads(line) for line in result.stdout.splitlines()]
    assert [line["event"] for line in lines] == ["hello", "log"]
    assert lines[0]["answer"] == 42
    assert lines[1]["message"] == "Application startup complete."
    assert "1.2.3.4" not in result.stdout + result.stderr
