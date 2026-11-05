"""WebSocket /v1/ws: presence, invites and friend pushes.

The client authenticates in its first frame, {"type": "auth", "token": "<access token>"}, within
10 seconds. Tokens are never accepted in the URL, where proxies and logs would keep them. The
socket closes with 4401 when that token expires, unless the client sends a fresh access token the
same way first; the server answers {"type": "auth_ok"}.
"""

import asyncio
import contextlib
import json
import uuid
from typing import Any

from fastapi import APIRouter, WebSocket
from pydantic import ValidationError
from sqlalchemy import select

from app.deps import Services
from app.log import log_event, logger
from app.models import User
from app.presence import (
    CLOSE_INTERNAL_ERROR,
    CLOSE_POLICY,
    CLOSE_TOO_BIG,
    CLOSE_UNAUTHORIZED,
    CLOSE_UNSUPPORTED_DATA,
    Connection,
    broadcast_presence,
    offline_event,
    presence_audience,
    visible_friend_presence,
)
from app.schemas import PresenceIn
from app.tokens import AccessClaims, TokenError, issued_before_cutoff

router = APIRouter()

MAX_MESSAGE_CHARS = 4096


def _parse(text: str | None) -> Any:
    if text is None or len(text) > MAX_MESSAGE_CHARS:
        return None
    try:
        return json.loads(text)
    except (ValueError, RecursionError):
        return None


def _error(code: str, message: str) -> dict[str, str]:
    return {"type": "error", "code": code, "message": message}


async def _close(websocket: WebSocket, code: int, reason: str) -> None:
    with contextlib.suppress(Exception):
        await websocket.close(code=code, reason=reason)


async def _check_token(token: Any, services: Services) -> AccessClaims | None:
    """The claims of a usable access token: valid, its account still there, and issued after the
    account's last password change. None otherwise. Database errors propagate."""
    if not isinstance(token, str):
        return None
    try:
        claims = services.tokens.decode(token)
    except TokenError:
        return None
    async with services.sessionmaker() as db:
        row = (
            await db.execute(
                select(User.id, User.tokens_valid_after).where(User.id == claims.user_id)
            )
        ).first()
    if row is None or issued_before_cutoff(claims.issued_at, row.tokens_valid_after):
        return None
    return claims


async def _authenticate(websocket: WebSocket, services: Services) -> AccessClaims | None:
    """The claims of the first frame's token, or None once the socket is closed.

    Return straight to the caller, which attaches the socket before its next await: a kick
    (sign-out, password change, deletion) that lands later then closes this socket too.
    """
    try:
        message = await asyncio.wait_for(
            websocket.receive(), timeout=services.tuning.ws_auth_timeout
        )
    except TimeoutError:
        await _close(websocket, CLOSE_POLICY, "auth timeout")
        return None
    if message["type"] == "websocket.disconnect":
        return None
    data = _parse(message.get("text"))
    if not isinstance(data, dict) or data.get("type") != "auth" or not isinstance(data.get("token"), str):
        await _close(websocket, CLOSE_POLICY, "first message must be auth")
        return None
    try:
        kicks = services.hub.kicks
        claims = await _check_token(data["token"], services)
        # A kick that came while that check ran closed the sockets the hub held then, not this
        # one; what it acted on has committed by now, so a second check sees it.
        while claims is not None and services.hub.kicks != kicks:
            kicks = services.hub.kicks
            claims = await _check_token(data["token"], services)
    except Exception:
        logger.exception("ws_auth_failed")
        await _close(websocket, CLOSE_INTERNAL_ERROR, "server error")
        return None
    if claims is None:
        await _close(websocket, CLOSE_UNAUTHORIZED, "invalid token")
        return None
    return claims


async def _renew(data: dict[str, Any], conn: Connection, services: Services) -> None:
    """A fresh access token for the same account keeps the socket open past the old one's expiry."""
    claims = await _check_token(data.get("token"), services)
    if claims is None or claims.user_id != conn.user_id:
        conn.close(CLOSE_UNAUTHORIZED, "invalid token")
        return
    conn.expire_at(claims.expires_at)
    conn.send({"type": "auth_ok"})


async def _handle(message: dict[str, Any], conn: Connection, services: Services) -> None:
    text = message.get("text")
    if text is None:
        conn.close(CLOSE_UNSUPPORTED_DATA, "binary frames are not supported")
        return
    if len(text) > MAX_MESSAGE_CHARS:
        conn.close(CLOSE_TOO_BIG, "message too big")
        return
    data = _parse(text)
    if not isinstance(data, dict):
        conn.send(_error("invalid_message", "Messages must be JSON objects."))
        return
    kind = data.get("type")
    if kind == "ping":
        conn.send({"type": "pong"})
    elif kind == "auth":
        await _renew(data, conn, services)
    elif kind == "presence":
        try:
            update = PresenceIn.model_validate(data)
        except ValidationError:
            conn.send(_error("invalid_presence", "Presence message is not valid."))
            return
        status = update.status
        activity = update.activity.model_dump() if update.activity is not None else None
        if (status, activity) == (conn.status, conn.activity):
            return  # nothing changed: no queries, nothing to send
        conn.status, conn.activity = status, activity
        if services.hub.connection(conn.user_id) is conn:
            async with services.sessionmaker() as db:
                await broadcast_presence(db, services.hub, conn.user_id)
    else:
        conn.send(_error("unknown_type", "Unknown message type."))


async def _announce_offline(services: Services, user_id: uuid.UUID) -> None:
    try:
        async with services.sessionmaker() as db:
            audience = await presence_audience(db, services.hub, user_id)
    except Exception:
        logger.exception("ws_offline_broadcast_failed")
        return
    services.hub.send_many(audience, offline_event(user_id))


@router.websocket("/v1/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    services: Services = websocket.app.state.services
    limiter = services.limiter
    await websocket.accept()
    # Sockets are limited per address until they sign in, then per account (README: Rate
    # limits); a socket that signs in gives its address's place back.
    address = "address:" + services.address_key(websocket.scope)
    if limiter.hit("ws_connect", address) > 0:
        await _close(websocket, CLOSE_POLICY, "too many connections")
        return
    claims = await _authenticate(websocket, services)
    if claims is None:
        return
    limiter.refund("ws_connect", address)
    user_id = claims.user_id
    if limiter.hit("ws_connect", f"user:{user_id}") > 0:
        await _close(websocket, CLOSE_POLICY, "too many connections")
        return
    hub = services.hub
    conn = hub.attach(user_id, websocket, expires_at=claims.expires_at)
    # No user id: the log would otherwise keep each account's online history, which presence
    # privacy keeps from everyone else.
    log_event("ws_connected", connections=len(hub))
    client_gone = False
    try:
        async with services.sessionmaker() as db:
            friends = await visible_friend_presence(db, hub, user_id)
            conn.send({"type": "hello", "user_id": str(user_id), "friends": friends})
            await broadcast_presence(db, hub, user_id)
        while True:
            message = await websocket.receive()
            if message["type"] == "websocket.disconnect":
                client_gone = True
                break
            if conn.closing:
                continue  # a close frame is on its way; wait for the client to go
            # Counted per account, so a new connection does not start a fresh allowance.
            if limiter.hit("ws_message", str(user_id)) > 0:
                conn.close(CLOSE_POLICY, "too many messages")
                continue
            try:
                await _handle(message, conn, services)
            except Exception:
                logger.exception("ws_message_failed")
                conn.close(CLOSE_INTERNAL_ERROR, "server error")
    except Exception:
        logger.exception("ws_failed")
        conn.close(CLOSE_INTERNAL_ERROR, "server error")
    finally:
        went_offline = await hub.detach(conn, client_gone=client_gone)
        if went_offline:
            await _announce_offline(services, user_id)
        log_event("ws_disconnected", connections=len(hub))
