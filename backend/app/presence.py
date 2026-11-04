"""In-memory presence hub behind /v1/ws.

Presence never touches the database and lives in this process only, which is one reason the
service runs as a single replica. Each connection has a bounded outbox drained by its own writer
task, so pushing to a slow client never blocks the request that triggered the push.
"""

import asyncio
import json
import time
import uuid
from collections.abc import Iterable
from typing import Any

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from starlette.websockets import WebSocket

from app.models import User
from app.social import block_partner_ids, friend_ids, is_blocked_either_way

# Close codes (documented in README.md).
CLOSE_NORMAL = 1000
CLOSE_GOING_AWAY = 1001
CLOSE_UNSUPPORTED_DATA = 1003
CLOSE_POLICY = 1008
CLOSE_TOO_BIG = 1009
CLOSE_INTERNAL_ERROR = 1011
CLOSE_TRY_AGAIN_LATER = 1013
CLOSE_REPLACED = 4000
CLOSE_UNAUTHORIZED = 4401
CLOSE_ACCOUNT_DELETED = 4410

OUTBOX_SIZE = 256


class _Close:
    __slots__ = ("code", "reason")

    def __init__(self, code: int, reason: str) -> None:
        self.code = code
        self.reason = reason


class Connection:
    def __init__(
        self, user_id: uuid.UUID, websocket: WebSocket, expires_at: float | None = None
    ) -> None:
        self.user_id = user_id
        self.websocket = websocket
        self.status = "online"
        self.activity: dict[str, Any] | None = None
        self.closing = False
        self._outbox: asyncio.Queue[str | _Close] = asyncio.Queue(maxsize=OUTBOX_SIZE)
        self._writer = asyncio.create_task(self._write_loop())
        self._expiry: asyncio.TimerHandle | None = None
        if expires_at is not None:
            self.expire_at(expires_at)

    def expire_at(self, expires_at: float) -> None:
        """Close with 4401 at `expires_at` (epoch seconds): the access token that opened, or last
        renewed, this socket expires then. A fresh token sent over the socket moves it on."""
        if self._expiry is not None:
            self._expiry.cancel()
        delay = max(0.0, expires_at - time.time())
        self._expiry = asyncio.get_running_loop().call_later(
            delay, self.close, CLOSE_UNAUTHORIZED, "token expired"
        )

    def presence(self) -> dict[str, Any]:
        return {"status": self.status, "activity": self.activity}

    def send(self, message: dict[str, Any]) -> bool:
        if self.closing:
            return False
        try:
            self._outbox.put_nowait(json.dumps(message, separators=(",", ":")))
        except asyncio.QueueFull:
            self.close(CLOSE_TRY_AGAIN_LATER, "too many unread messages")
            return False
        return True

    def close(self, code: int, reason: str = "") -> None:
        """Queue a close frame; later sends are dropped."""
        if self.closing:
            return
        self.closing = True
        while True:
            try:
                self._outbox.put_nowait(_Close(code, reason))
                return
            except asyncio.QueueFull:
                self._outbox.get_nowait()

    async def _write_loop(self) -> None:
        try:
            while True:
                item = await self._outbox.get()
                if isinstance(item, _Close):
                    await self.websocket.close(code=item.code, reason=item.reason)
                    return
                await self.websocket.send_text(item)
        except Exception:
            # The client is gone; the reader sees the disconnect and cleans up.
            self.closing = True

    async def finish(self, *, client_gone: bool) -> None:
        if self._expiry is not None:
            self._expiry.cancel()
        if client_gone:
            self.closing = True
            self._writer.cancel()
        else:
            self.close(CLOSE_NORMAL)
        done, _ = await asyncio.wait({self._writer}, timeout=2)
        if not done:  # a client too slow to take the close frame
            self._writer.cancel()


class PresenceHub:
    def __init__(self) -> None:
        self._connections: dict[uuid.UUID, Connection] = {}
        # Counts of kicks and of new blocks. Work that reads the database, then acts on what it
        # read (a sign-in attaching its socket, a push choosing its audience), compares them
        # before and after and reads again if one moved: the change committed in between.
        self.kicks = 0
        self.blocks = 0

    def connection(self, user_id: uuid.UUID) -> Connection | None:
        return self._connections.get(user_id)

    def is_online(self, user_id: uuid.UUID) -> bool:
        return user_id in self._connections

    def presence(self, user_id: uuid.UUID) -> dict[str, Any] | None:
        conn = self._connections.get(user_id)
        return None if conn is None else conn.presence()

    def attach(
        self, user_id: uuid.UUID, websocket: WebSocket, expires_at: float | None = None
    ) -> Connection:
        """Register a connection; one per user, so an older one is closed."""
        conn = Connection(user_id, websocket, expires_at)
        previous = self._connections.get(user_id)
        self._connections[user_id] = conn
        if previous is not None:
            previous.close(CLOSE_REPLACED, "replaced by a newer connection")
        return conn

    async def detach(self, conn: Connection, *, client_gone: bool) -> bool:
        """Forget a connection. True when it was the user's current one (so they went offline)."""
        current = self._connections.get(conn.user_id) is conn
        if current:
            del self._connections[conn.user_id]
        await conn.finish(client_gone=client_gone)
        return current

    def send(self, user_id: uuid.UUID, message: dict[str, Any]) -> bool:
        conn = self._connections.get(user_id)
        return conn.send(message) if conn is not None else False

    def send_many(self, user_ids: Iterable[uuid.UUID], message: dict[str, Any]) -> None:
        for user_id in user_ids:
            self.send(user_id, message)

    def kick(self, user_id: uuid.UUID, code: int, reason: str) -> None:
        """Close the user's socket. Call it after the change that requires it has committed."""
        self.kicks += 1
        conn = self._connections.get(user_id)
        if conn is not None:
            conn.close(code, reason)

    def note_block(self) -> None:
        """A block has just committed: anything that read block state before must read again."""
        self.blocks += 1

    async def close_all(self, code: int = CLOSE_GOING_AWAY, reason: str = "server restarting") -> None:
        connections = list(self._connections.values())
        for conn in connections:
            conn.close(code, reason)
        writers = [conn._writer for conn in connections]
        if writers:
            await asyncio.wait(writers, timeout=2)

    def __len__(self) -> int:
        return len(self._connections)


def presence_event(user_id: uuid.UUID, presence: dict[str, Any]) -> dict[str, Any]:
    return {"type": "presence", "user_id": str(user_id), **presence}


def offline_event(user_id: uuid.UUID) -> dict[str, Any]:
    return {"type": "offline", "user_id": str(user_id)}


async def presence_audience(
    db: AsyncSession,
    hub: PresenceHub,
    user_id: uuid.UUID,
    *,
    respect_privacy: bool = True,
) -> list[uuid.UUID]:
    """Online friends allowed to see `user_id`'s presence (never anyone in a block).

    Send to them before the next await: a block that commits while this reads makes it read
    again (PresenceHub.blocks), so nothing sent after a block reaches the blocker.
    """
    while True:
        blocks = hub.blocks
        audience = await _audience(db, hub, user_id, respect_privacy)
        if hub.blocks == blocks:
            return audience


async def _audience(
    db: AsyncSession, hub: PresenceHub, user_id: uuid.UUID, respect_privacy: bool
) -> list[uuid.UUID]:
    if respect_privacy:
        privacy = await db.scalar(select(User.privacy_presence).where(User.id == user_id))
        if privacy != "friends":
            return []
    online = {friend for friend in await friend_ids(db, user_id) if hub.is_online(friend)}
    if online:
        online -= await block_partner_ids(db, user_id)
    return sorted(online)


async def visible_friend_presence(
    db: AsyncSession, hub: PresenceHub, user_id: uuid.UUID
) -> list[dict[str, Any]]:
    """Presence of `user_id`'s online friends who let friends see it (as presence_audience)."""
    while True:
        blocks = hub.blocks
        online = {friend for friend in await friend_ids(db, user_id) if hub.is_online(friend)}
        if online:
            online -= await block_partner_ids(db, user_id)
        visible = []
        if online:
            visible = list(
                await db.scalars(
                    select(User.id).where(
                        User.id.in_(sorted(online)), User.privacy_presence == "friends"
                    )
                )
            )
        if hub.blocks == blocks:
            break
    result = []
    for friend in visible:
        presence = hub.presence(friend)
        if presence is not None:
            result.append({"user_id": str(friend), **presence})
    return result


async def broadcast_presence(db: AsyncSession, hub: PresenceHub, user_id: uuid.UUID) -> None:
    if hub.presence(user_id) is None:
        return
    audience = await presence_audience(db, hub, user_id)
    # Read after the await, so what goes out is the newest, even if it changed meanwhile.
    presence = hub.presence(user_id)
    if presence is not None:
        hub.send_many(audience, presence_event(user_id, presence))


async def show_presence_between(
    db: AsyncSession, hub: PresenceHub, a: uuid.UUID, b: uuid.UUID
) -> None:
    """Two users just became friends: show each the other's presence where allowed."""
    while True:
        blocks = hub.blocks
        visible = set(
            await db.scalars(
                select(User.id).where(User.id.in_((a, b)), User.privacy_presence == "friends")
            )
        )
        if hub.blocks == blocks:
            break
        if await is_blocked_either_way(db, a, b):  # a block came in between, and wins
            return
    for viewer, subject in ((a, b), (b, a)):
        presence = hub.presence(subject)
        if subject in visible and presence is not None and hub.is_online(viewer):
            hub.send(viewer, presence_event(subject, presence))


async def blocked_now(db: AsyncSession, hub: PresenceHub, a: uuid.UUID, b: uuid.UUID) -> bool:
    """Whether either user blocks the other, read after every block the hub has been told of.

    For a request, invite or friendship that checked for a block, was written, and is about to be
    pushed: a block that committed in between must win. Push before the next await.
    """
    while True:
        blocks = hub.blocks
        if await is_blocked_either_way(db, a, b):
            return True
        if hub.blocks == blocks:
            return False


async def hide_presence_between(
    db: AsyncSession, hub: PresenceHub, a: uuid.UUID, b: uuid.UUID
) -> None:
    """A friendship just ended (unfriend or block): each side stops seeing the other.

    Call it only when a friendship really was removed. An `offline` event goes only to a viewer who
    could see the subject until now: anyone else would learn from it that the subject is online.
    """
    for viewer, subject in ((a, b), (b, a)):
        if not hub.is_online(viewer) or not hub.is_online(subject):
            continue
        privacy = await db.scalar(select(User.privacy_presence).where(User.id == subject))
        if privacy == "friends":
            hub.send(viewer, offline_event(subject))
