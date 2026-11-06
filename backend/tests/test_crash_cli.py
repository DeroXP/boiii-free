"""scripts/crashes.py, the owner's command line for crash reports, against the app in-process."""

import ast
import asyncio
import base64
import http.server
import importlib.util
import io
import sys
import threading
from collections.abc import Mapping
from datetime import UTC, datetime
from pathlib import Path
from types import ModuleType
from typing import Any

import httpx
import pytest
from fastapi import FastAPI

from tests.crashdata import ADMIN_TOKEN, crash_report, minidump, minimal_report, post_crash
from tests.helpers import AppFactory, client_for, make_settings

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "crashes.py"
BASE = "http://127.0.0.1:8000"


def _load() -> ModuleType:
    spec = importlib.util.spec_from_file_location("xerces_crashes_cli", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cli = _load()


def _bridge(client: httpx.AsyncClient, loop: asyncio.AbstractEventLoop, seen: list[str]) -> Any:
    """A fetch for the CLI (which runs in a thread) that sends through the test's client."""

    def fetch(url: str, headers: Mapping[str, str]) -> tuple[int, Mapping[str, str], bytes]:
        seen.append(url)

        async def go() -> httpx.Response:
            return await client.get(url, headers={**headers, "X-Forwarded-For": "192.0.2.10"})

        response = asyncio.run_coroutine_threadsafe(go(), loop).result(10)
        return response.status_code, dict(response.headers), response.content

    return fetch


async def _run(
    client: httpx.AsyncClient,
    *argv: str,
    env: Mapping[str, str] | None = None,
    seen: list[str] | None = None,
) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    environment = {"XERCES_BACKEND_URL": BASE, "XERCES_ADMIN_TOKEN": ADMIN_TOKEN} if env is None else env
    fetch = _bridge(client, asyncio.get_running_loop(), seen if seen is not None else [])
    code = await asyncio.to_thread(
        cli.main, list(argv), env=environment, fetch=fetch, out=out, err=err
    )
    return code, out.getvalue(), err.getvalue()


@pytest.fixture
async def admin_app(app_factory: AppFactory, tmp_path: Path) -> FastAPI:
    return await app_factory(settings=make_settings(tmp_path, XERCES_ADMIN_TOKEN=ADMIN_TOKEN))


async def test_groups_list_show_and_minidump(
    admin_app: FastAPI, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    async with client_for(admin_app) as client:
        dump = minidump(b"stack bytes")
        report = crash_report(minidump_b64=base64.b64encode(dump).decode())
        crash_id = (await post_crash(client, report)).json()["id"]
        bare = (await post_crash(client, minimal_report())).json()["id"]

        code, out, err = await _run(client, "groups", "--since", "7d")
        assert (code, err) == (0, "")
        assert "2 reports in 2 groups since" in out
        assert "BlackOps3.exe+0x1a2b3c" in out and "Weapon Pack (1)" in out
        assert f"sample    {crash_id}" in out

        seen: list[str] = []
        code, out, err = await _run(client, "list", "--limit", "1", "--all", seen=seen)
        assert (code, err) == (0, "")
        assert crash_id in out and bare in out
        assert len(seen) == 2 and "cursor=" in seen[1]  # followed the second page
        code, out, _ = await _run(client, "list", "--limit", "1")
        assert "(more: add --all)" in out

        code, out, err = await _run(client, "show", crash_id)
        assert (code, err) == (0, "")
        for text in ("signature", "0xC0000005", "#0   BlackOps3.exe+0x1a2b3c", "zm_castle",
                     "RTX 5070 Ti", "error: something broke", "== Journal"):
            assert text in out, text
        code, out, _ = await _run(client, "show", crash_id, "--json")
        assert code == 0 and '"report_id"' in out

        monkeypatch.chdir(tmp_path)
        code, out, err = await _run(client, "minidump", crash_id)
        assert (code, err) == (0, "")
        assert (tmp_path / f"{crash_id}.dmp").read_bytes() == dump
        code, _, err = await _run(client, "minidump", crash_id)
        assert code == 2 and "--force" in err
        assert (await _run(client, "minidump", crash_id, "--force"))[0] == 0
        target = tmp_path / "named.dmp"
        assert (await _run(client, "minidump", crash_id, "-o", str(target)))[0] == 0
        assert target.read_bytes() == dump

        code, _, err = await _run(client, "minidump", bare, "-o", str(tmp_path / "none.dmp"))
        assert code == 1 and "no minidump" in err
        assert not (tmp_path / "none.dmp").exists()


async def test_setup_errors(admin_app: FastAPI, app_factory: AppFactory, tmp_path: Path) -> None:
    (tmp_path / "plain").mkdir()
    plain_app = await app_factory(settings=make_settings(tmp_path / "plain"))
    async with client_for(admin_app) as admin:
        code, _, err = await _run(admin, "groups", env={"XERCES_ADMIN_TOKEN": ADMIN_TOKEN})
        assert code == 2 and "XERCES_BACKEND_URL" in err
        code, _, err = await _run(admin, "groups", env={"XERCES_BACKEND_URL": BASE})
        assert code == 2 and "XERCES_ADMIN_TOKEN" in err
        plain = {"XERCES_BACKEND_URL": "http://example.com", "XERCES_ADMIN_TOKEN": ADMIN_TOKEN}
        seen: list[str] = []
        code, _, err = await _run(admin, "groups", env=plain, seen=seen)
        assert code == 2 and "https://" in err and seen == []  # the token was never sent
        wrong = {"XERCES_BACKEND_URL": BASE, "XERCES_ADMIN_TOKEN": "w" * 40}
        code, _, err = await _run(admin, "groups", env=wrong)
        assert code == 1 and "401" in err and "w" * 40 not in err
    # A server without XERCES_ADMIN_TOKEN answers 404, and the message says what that can mean.
    async with client_for(plain_app) as client:
        code, _, err = await _run(client, "list")
    assert code == 1 and "XERCES_ADMIN_TOKEN is not set on the server" in err


class _FakeBackend(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802 (the name http.server calls)
        if self.headers.get("Authorization") != f"Bearer {ADMIN_TOKEN}":
            body, status = b'{"error": {"code": "unauthorized", "message": "no"}}', 401
        elif self.path.startswith("/v1/admin/crash-groups?limit=50"):
            body, status = b'{"reports": 0, "groups": []}', 200
        else:
            body, status = b'{"error": {"code": "not_found", "message": "Not Found"}}', 404
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args: Any) -> None:
        pass


def test_urllib_against_a_local_server() -> None:
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FakeBackend)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_address[1]}"
        out, err = io.StringIO(), io.StringIO()
        env = {"XERCES_BACKEND_URL": base, "XERCES_ADMIN_TOKEN": ADMIN_TOKEN}
        assert cli.main(["groups"], env=env, out=out, err=err) == 0, err.getvalue()
        assert "0 reports in 0 groups" in out.getvalue()
        wrong = {"XERCES_BACKEND_URL": base, "XERCES_ADMIN_TOKEN": "x" * 40}
        assert cli.main(["groups"], env=wrong, out=out, err=err) == 1
        assert "401" in err.getvalue()
        assert cli.main(["show", "nope"], env=env, out=out, err=err) == 1
        assert "404" in err.getvalue()
    finally:
        server.shutdown()
        server.server_close()


def test_unreachable_server() -> None:
    def fetch(url: str, headers: Mapping[str, str]) -> Any:
        raise OSError("connection refused")

    err = io.StringIO()
    env = {"XERCES_BACKEND_URL": "https://backend.invalid", "XERCES_ADMIN_TOKEN": ADMIN_TOKEN}
    code = cli.main(["groups"], env=env, fetch=fetch, out=io.StringIO(), err=err)
    assert code == 1 and "Could not reach https://backend.invalid" in err.getvalue()


def test_since() -> None:
    now = datetime(2026, 11, 5, 12, 0, tzinfo=UTC)
    assert cli.parse_since("7d", now) == "2026-10-29T12:00:00Z"
    assert cli.parse_since("12h", now) == "2026-11-05T00:00:00Z"
    assert cli.parse_since("30m", now) == "2026-11-05T11:30:00Z"
    assert cli.parse_since("2026-11-01", now) == "2026-11-01T00:00:00Z"
    assert cli.parse_since("2026-11-01T10:00:00+02:00", now) == "2026-11-01T08:00:00Z"
    assert cli.parse_since(None) is None
    with pytest.raises(cli.CliError):
        cli.parse_since("last week")


def test_the_script_uses_only_the_standard_library() -> None:
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"))
    modules = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            modules.add(node.module.split(".")[0])
    assert modules and modules <= set(sys.stdlib_module_names), modules - set(sys.stdlib_module_names)
