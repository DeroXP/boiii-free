"""Request hygiene: body size caps, security headers, request logs and hashed client addresses.

All three middlewares are plain ASGI so they see every response, including 4xx errors. A 500 from
an unhandled exception is sent by Starlette's ServerErrorMiddleware, which sits outside them, so
app.errors adds the same headers to it and RequestLogMiddleware records it as status 500.
"""

import asyncio
import hashlib
import hmac
import ipaddress
import logging
import time
from collections.abc import Mapping

from starlette.datastructures import MutableHeaders
from starlette.types import ASGIApp, Message, Receive, Scope, Send

from app.errors import API_CONTENT_SECURITY_POLICY, SECURITY_HEADERS, error_response
from app.log import log_event


def client_address(scope: Scope, trust_proxy: bool, proxy_hops: int = 0) -> str:
    """The caller's address, for rate limiting only (it is hashed, never stored or logged).

    Behind Railway's edge (XERCES_TRUST_PROXY=1) it comes from X-Forwarded-For: the first entry
    when the edge replaces the header (proxy_hops 0), else the entry `proxy_hops` places from the
    right, which is the address the outermost of that many appending proxies saw. Entries further
    left came from the client and prove nothing.
    """
    if trust_proxy:
        entries = [
            entry.strip()
            for name, value in scope.get("headers", ())
            if name == b"x-forwarded-for"
            for entry in value.decode("latin-1").split(",")
        ]
        entries = [entry for entry in entries if entry]
        if entries:
            if proxy_hops <= 0:
                return _normalize_address(entries[0])
            return _normalize_address(entries[-min(proxy_hops, len(entries))])
    client = scope.get("client")
    if client and client[0]:
        return _normalize_address(str(client[0]))
    return "unknown"


def _normalize_address(value: str) -> str:
    text = value.strip()
    ip = _parse_ip(text)
    if ip is None:
        return text.lower()[:100]
    if isinstance(ip, ipaddress.IPv6Address):
        if ip.ipv4_mapped is not None:
            return str(ip.ipv4_mapped)
        # One person usually holds a whole /64, so limit the prefix rather than each address.
        return str(ipaddress.IPv6Network((ip, 64), strict=False))
    return str(ip)


def _parse_ip(text: str) -> ipaddress.IPv4Address | ipaddress.IPv6Address | None:
    candidates = [text]
    if text.startswith("[") and "]" in text:  # [v6]:port
        candidates.append(text[1 : text.index("]")])
    elif text.count(":") == 1:  # v4:port
        candidates.append(text.split(":", 1)[0])
    for candidate in candidates:
        try:
            return ipaddress.ip_address(candidate)
        except ValueError:
            continue
    return None


def hashed_address(secret: bytes, address: str) -> str:
    return hmac.new(secret, address.encode("utf-8"), hashlib.sha256).hexdigest()[:32]


class BodySizeLimitMiddleware:
    """Reject bodies over `max_bytes` before anything parses them (413).

    Paths in `large_paths` (POST /v1/crashes) get their own, larger limit. At most `large_slots`
    of those are read and handled at once, so a burst of big uploads cannot exhaust memory; the
    rest are turned away with 503 and Retry-After. Reading one may take `large_timeout` seconds,
    so slow senders cannot hold the slots (408).
    """

    def __init__(
        self,
        app: ASGIApp,
        max_bytes: int = 128 * 1024,
        *,
        large_paths: Mapping[str, int] | None = None,
        large_slots: int = 8,
        large_timeout: float = 30.0,
    ) -> None:
        self.app = app
        self.max_bytes = max_bytes
        self.large_paths = dict(large_paths or {})
        self.large_slots = large_slots
        self.large_timeout = large_timeout
        self._large_in_flight = 0

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        limit = self.large_paths.get(scope.get("path", ""))
        if limit is None:
            await self._handle(scope, receive, send, self.max_bytes, None)
            return
        if self._large_in_flight >= self.large_slots:
            response = error_response(
                503,
                "unavailable",
                "Too many uploads at once. Try again shortly.",
                headers={"Retry-After": "10"},
            )
            await response(scope, receive, send)
            return
        self._large_in_flight += 1
        try:
            await self._handle(scope, receive, send, limit, self.large_timeout)
        finally:
            self._large_in_flight -= 1

    async def _handle(
        self, scope: Scope, receive: Receive, send: Send, limit: int, timeout: float | None
    ) -> None:
        for name, value in scope.get("headers", ()):
            if name == b"content-length":
                try:
                    declared = int(value)
                except ValueError:
                    await self._reject(scope, receive, send, 400, limit)
                    return
                if declared > limit:
                    await self._reject(scope, receive, send, 413, limit)
                    return
        # Read the body here (bounded) so chunked uploads cannot slip past the limit.
        body = bytearray()
        too_large = False
        try:
            async with asyncio.timeout(timeout):
                while True:
                    message = await receive()
                    if message["type"] != "http.request":
                        return  # client went away
                    body.extend(message.get("body", b""))
                    if len(body) > limit:
                        too_large = True
                        break
                    if not message.get("more_body", False):
                        break
        except TimeoutError:
            await self._reject(scope, receive, send, 408, limit)
            return
        if too_large:
            await self._reject(scope, receive, send, 413, limit)
            return

        replayed = False

        async def replay() -> Message:
            nonlocal replayed
            if not replayed:
                replayed = True
                return {"type": "http.request", "body": bytes(body), "more_body": False}
            return await receive()

        await self.app(scope, replay, send)

    async def _reject(
        self, scope: Scope, receive: Receive, send: Send, status: int, limit: int
    ) -> None:
        if status == 413:
            response = error_response(
                413, "payload_too_large", f"Request body is larger than {size_text(limit)}."
            )
        elif status == 408:
            response = error_response(408, "request_timeout", "The request body came too slowly.")
        else:
            response = error_response(400, "bad_request", "Content-Length is not a number.")
        await response(scope, receive, send)


def size_text(limit: int) -> str:
    """128 KB, 4 MiB: how the limits are written in error messages and the README."""
    if limit >= 1024 * 1024 and limit % (1024 * 1024) == 0:
        return f"{limit // (1024 * 1024)} MiB"
    return f"{limit // 1024} KB"


class SecurityHeadersMiddleware:
    def __init__(self, app: ASGIApp) -> None:
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        path: str = scope.get("path", "")
        api_only = not path.startswith(("/docs", "/openapi.json"))

        async def send_with_headers(message: Message) -> None:
            if message["type"] == "http.response.start":
                headers = MutableHeaders(scope=message)
                # nosniff, and no-store: tokens and account data must never sit in a cache.
                for name, value in SECURITY_HEADERS.items():
                    headers.setdefault(name, value)
                if api_only:
                    headers.setdefault("Content-Security-Policy", API_CONTENT_SECURITY_POLICY)
            await send(message)

        await self.app(scope, receive, send_with_headers)


class RequestLogMiddleware:
    """One JSON line per request: method, route template, status, duration. No address, no agent."""

    def __init__(self, app: ASGIApp) -> None:
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        started = time.perf_counter()
        status = 500

        async def send_and_record(message: Message) -> None:
            nonlocal status
            if message["type"] == "http.response.start":
                status = message["status"]
            await send(message)

        try:
            await self.app(scope, receive, send_and_record)
        finally:
            route = getattr(scope.get("route"), "path", None) or "unmatched"
            if not (route == "/healthz" and status == 200):
                log_event(
                    "request",
                    logging.WARNING if status >= 500 else logging.INFO,
                    method=scope.get("method"),
                    route=route,
                    status=status,
                    ms=round((time.perf_counter() - started) * 1000, 1),
                )
