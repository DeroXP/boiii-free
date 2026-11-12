"""Crash report test data: a full xerces.crash/1 report, a fake minidump and a sender."""

import base64
import copy
import gzip
import json
import secrets
from typing import Any

import httpx

from tests.helpers import next_address

ADMIN_TOKEN = "admin-token-for-tests-" + "t" * 32
INSTALL = "0123456789abcdef0123456789abcdef"


def new_hex_id() -> str:
    return secrets.token_hex(16)


def minidump(*parts: bytes) -> bytes:
    """Starts like a real minidump (signature, version); the rest is whatever a test needs."""
    return b"MDMP\x93\xa7\x00\x00" + b"\x00" * 24 + b"".join(parts)


def utf16(text: str) -> bytes:
    return text.encode("utf-16-le")


def crash_report(**overrides: Any) -> dict[str, Any]:
    report: dict[str, Any] = {
        "schema": "xerces.crash/1",
        "report_id": new_hex_id(),
        "created_utc": "2026-11-05T12:00:00Z",
        "xerces": {"version": "0.1.0-beta.1", "commit": "a32259cb", "dirty": False},
        "game": {"build": "0x6531394", "steam_build": 24784313},
        "system": {
            "windows": "10.0.26300",
            "cpu": "Intel(R) Core(TM) Ultra 9 285K",
            "logical_cpus": 24,
            "ram_mib": 65536,
            "gpus": [{"name": "NVIDIA GeForce RTX 5070 Ti", "driver": "591.44"}],
            "display": "3840x2160@240",
        },
        "launch": {
            "mode": "zm",
            "map": "zm_castle",
            "plan_id": "plan-7",
            "mods": [{"id": "ws.1944930083", "name": "Weapon Pack", "version": "1.2"}],
            "flags": ["-xerces-db-pool"],
            "safe_mode": False,
        },
        "crash": {
            "kind": "exception",
            "code": "0xC0000005",
            "exit_code": None,
            "module": "BlackOps3.exe",
            "offset": "0x1a2b3c",
            "thread": "main",
            "frames": [
                "BlackOps3.exe+0x1a2b3c",
                "BlackOps3.exe+0x2000",
                "boiii.exe+0x400",
                "ntdll.dll+0x10",
            ],
            "phase": "in_game",
            "reason": "exception",
            "summary": "invalid memory access: tried to read 0x18 in BlackOps3.exe+0x1a2b3c",
            "script": "GSC zm_castle::main (scripts/zm/zm_castle.gsc:42)",
        },
        "logs": {
            "journal": {"phase": "in_game", "clean_exit": False, "plan_id": "plan-7"},
            "game_log_tail": "loading zm_castle\nerror: something broke",
            "launcher_log_tail": "launcher started the game",
            "console_tail": "]map zm_castle",
            "details": "Xerces Crash Report\n\nWhat happened: invalid memory access\nMap: zm_castle",
        },
        "minidump_b64": base64.b64encode(minidump(b"stack")).decode("ascii"),
    }
    for key, value in overrides.items():
        report[key] = value
    return copy.deepcopy(report)


def minimal_report(**overrides: Any) -> dict[str, Any]:
    report: dict[str, Any] = {
        "schema": "xerces.crash/1",
        "report_id": new_hex_id(),
        "created_utc": "2026-11-05T12:00:00Z",
        "xerces": {"version": "0.1.0-beta.1"},
        "crash": {"kind": "unknown_exit"},
    }
    report.update(overrides)
    return report


async def post_crash(
    client: httpx.AsyncClient,
    report: Any,
    *,
    install: str | None = INSTALL,
    address: str | None = None,
    compress: bool = False,
    headers: dict[str, str] | None = None,
) -> httpx.Response:
    body = report if isinstance(report, bytes) else json.dumps(report).encode("utf-8")
    sent = {
        "Content-Type": "application/json",
        "User-Agent": "Xerces/0.1.0-beta.1",
        "X-Forwarded-For": address or next_address(),
    }
    if install is not None:
        sent["X-Xerces-Install"] = install
    if compress:
        body = gzip.compress(body)
        sent["Content-Encoding"] = "gzip"
    sent.update(headers or {})
    return await client.post("/v1/crashes", content=body, headers=sent)


def admin_headers(token: str = ADMIN_TOKEN, address: str | None = None) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}", "X-Forwarded-For": address or next_address()}
