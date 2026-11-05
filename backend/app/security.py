"""Request hygiene: body size cap, security headers, request logs and hashed client addresses.

All three middlewares are plain ASGI so they see every response, including 4xx errors. A 500 from
an unhandled exception is sent by Starlette's ServerErrorMiddleware, which sits outside them, so
app.errors adds the same headers to it and RequestLogMiddleware records it as status 500.
"""

import hashlib
import hmac
import ipaddress
import logging
import time

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
    """Reject bodies over `max_bytes` before anything parses them (413)."""

    def __init__(self, app: ASGIApp, max_bytes: int = 128 * 1024) -> None:
        self.app = app
        self.max_bytes = max_bytes

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        for name, value in scope.get("headers", ()):
            if name == b"content-length":
                try:
                    declared = int(value)
                except ValueError:
                    await self._reject(scope, receive, send, 400)
                    return
                if declared > self.max_bytes:
                    await self._reject(scope, receive, send, 413)
                    return
        # Read the body here (bounded) so chunked uploads cannot slip past the limit.
        body = bytearray()
        while True:
            message = await receive()
            if message["type"] != "http.request":
                return  # client went away
            body.extend(message.get("body", b""))
            if len(body) > self.max_bytes:
                await self._reject(scope, receive, send, 413)
                return
            if not message.get("more_body", False):
                break

        replayed = False

        async def replay() -> Message:
            nonlocal replayed
            if not replayed:
                replayed = True
                return {"type": "http.request", "body": bytes(body), "more_body": False}
            return await receive()

        await self.app(scope, replay, send)

    async def _reject(self, scope: Scope, receive: Receive, send: Send, status: int) -> None:
        if status == 413:
            response = error_response(
                413,
                "payload_too_large",
                f"Request body is larger than {self.max_bytes // 1024} KB.",
            )
        else:
            response = error_response(400, "bad_request", "Content-Length is not a number.")
        await response(scope, receive, send)


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
