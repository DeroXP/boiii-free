"""Argon2id password hashing, off the event loop, with a cap on concurrent and queued hashes.

Each hash takes `memory_kib` of RAM, so the semaphore is what stops a login flood from exhausting
memory, and the queue limit is what stops it from making every sign-in wait for minutes. Passwords
are NFKC-normalized first so the same password typed on different systems produces the same hash.
"""

import asyncio
import contextlib
import secrets
import unicodedata
from collections.abc import Callable, Iterator
from typing import TypeVar

from argon2 import PasswordHasher, Type
from argon2.exceptions import InvalidHashError, VerificationError

from app.errors import ApiError

T = TypeVar("T")


def normalize_password(password: str) -> str:
    return unicodedata.normalize("NFKC", password)


class PasswordService:
    def __init__(
        self,
        *,
        time_cost: int,
        memory_kib: int,
        parallelism: int,
        max_concurrency: int = 4,
        max_waiting: int = 64,
    ) -> None:
        self._hasher = PasswordHasher(
            time_cost=time_cost,
            memory_cost=memory_kib,
            parallelism=parallelism,
            hash_len=32,
            salt_len=16,
            type=Type.ID,
        )
        self._slots = asyncio.Semaphore(max_concurrency)
        self._max_admitted = max_concurrency + max_waiting
        self._admitted = 0
        self._dummy_hash: str | None = None

    def ensure_room(self) -> None:
        """Answer 503 now if the line is full, as `admit` would.

        Logins and sign-ups call it before they spend their address's rate limit, so a launcher
        that retries while a flood fills the line does not use up its own allowance.
        """
        if self._admitted >= self._max_admitted:
            raise ApiError(
                503,
                "busy",
                "The server is busy. Try again in a moment.",
                headers={"Retry-After": "1"},
            )

    @contextlib.contextmanager
    def admit(self) -> Iterator[None]:
        """Hold a place in line for one request's hashes, or answer 503 when the line is full.

        Logins and sign-ups take it before they touch the database, so one turned away costs
        nothing: a flood of made-up logins fills the line and then bounces off it, while every
        other route keeps its connections and its speed.
        """
        self.ensure_room()
        self._admitted += 1
        try:
            yield
        finally:
            self._admitted -= 1

    async def _run(self, func: Callable[..., T], *args: object) -> T:
        async with self._slots:
            return await asyncio.to_thread(func, *args)

    async def hash(self, password: str) -> str:
        return await self._run(self._hasher.hash, normalize_password(password))

    async def verify(self, password_hash: str, password: str) -> bool:
        return await self._run(self._verify_sync, password_hash, normalize_password(password))

    def _verify_sync(self, password_hash: str, password: str) -> bool:
        try:
            return self._hasher.verify(password_hash, password)
        except (VerificationError, InvalidHashError):
            return False

    def needs_rehash(self, password_hash: str) -> bool:
        try:
            return self._hasher.check_needs_rehash(password_hash)
        except InvalidHashError:
            return True

    async def warm_up(self) -> None:
        if self._dummy_hash is None:
            self._dummy_hash = await self.hash(secrets.token_urlsafe(24))

    async def burn(self, password: str) -> None:
        """Do the work of a real check, so an unknown username costs the same time."""
        await self.warm_up()
        assert self._dummy_hash is not None
        await self.verify(self._dummy_hash, password)
