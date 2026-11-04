"""In-memory token-bucket rate limits.

State lives in this process only, which is why the service runs as a single replica. Keys are user
ids or HMACs of client addresses (see app.security); raw addresses never reach this module.
"""

import math
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass

from app.errors import ApiError


@dataclass(frozen=True)
class Rule:
    """`capacity` requests per `period` seconds, refilled smoothly."""

    capacity: float
    period: float

    @property
    def rate(self) -> float:
        return self.capacity / self.period


HOUR = 3600.0
DAY = 86400.0

DEFAULT_RULES: Mapping[str, Rule] = {
    "register": Rule(3, HOUR),  # every attempt (taken names too), per hashed address
    "login_address": Rule(10, 15 * 60),  # every login attempt, per hashed address
    "login_username": Rule(5, 15 * 60),  # failed logins, per username (refunded on success)
    "refresh": Rule(30, 60),  # per user (per hashed address for unusable tokens)
    "friend_request": Rule(20, DAY),  # per user
    "invite": Rule(30, HOUR),  # per user
    "invite_recipient": Rule(1, 30),  # per sender and recipient
    "report": Rule(10, DAY),  # per user
    "pack_create": Rule(20, DAY),  # per user
    "pack_fetch": Rule(120, 60),  # per hashed address (codes are public, this stops scraping)
    "password_check": Rule(5, 15 * 60),  # wrong passwords on /v1/me, per user (refunded if right)
    "ws_message": Rule(10, 1),  # per WebSocket connection
}


def rate_limited(wait_seconds: float) -> ApiError:
    retry_after = str(max(1, math.ceil(wait_seconds)))
    return ApiError(
        429,
        "rate_limited",
        "Too many requests. Try again later.",
        headers={"Retry-After": retry_after},
    )


class TokenBucket:
    """A single bucket, used directly for per-connection WebSocket limits."""

    __slots__ = ("_clock", "rule", "tokens", "updated")

    def __init__(self, rule: Rule, clock: Callable[[], float] = time.monotonic) -> None:
        self.rule = rule
        self._clock = clock
        self.tokens = float(rule.capacity)
        self.updated = clock()

    def _refill(self, now: float) -> None:
        elapsed = max(0.0, now - self.updated)
        self.tokens = min(self.rule.capacity, self.tokens + elapsed * self.rule.rate)
        self.updated = now

    def take(self, cost: float = 1.0) -> bool:
        self._refill(self._clock())
        if self.tokens >= cost:
            self.tokens -= cost
            return True
        return False


class RateLimiter:
    def __init__(
        self,
        rules: Mapping[str, Rule] | None = None,
        *,
        clock: Callable[[], float] = time.monotonic,
        max_buckets: int = 100_000,
    ) -> None:
        self.rules: dict[str, Rule] = dict(DEFAULT_RULES)
        if rules:
            self.rules.update(rules)
        self._clock = clock
        self._max_buckets = max_buckets
        self._buckets: dict[tuple[str, str], TokenBucket] = {}

    def _bucket(self, name: str, key: str) -> TokenBucket:
        bucket = self._buckets.get((name, key))
        if bucket is None:
            if len(self._buckets) >= self._max_buckets:
                self.prune()
            bucket = TokenBucket(self.rules[name], self._clock)
            self._buckets[(name, key)] = bucket
        return bucket

    def hit(self, name: str, key: str, cost: float = 1.0) -> float:
        """Spend `cost` tokens. Returns 0 when allowed, else the seconds until it would be."""
        bucket = self._bucket(name, key)
        if bucket.take(cost):
            return 0.0
        return (cost - bucket.tokens) / bucket.rule.rate

    def refund(self, name: str, key: str, amount: float = 1.0) -> None:
        """Give back what `hit` spent, for attempts that turn out not to count (a right password).

        Spend first and refund later, never check first and spend later: requests that arrive
        while an earlier one is still awaiting (a password hash) would all pass the check.
        """
        bucket = self._buckets.get((name, key))
        if bucket is not None:
            bucket.tokens = min(bucket.rule.capacity, bucket.tokens + amount)

    def enforce(self, name: str, key: str, cost: float = 1.0) -> None:
        """Spend `cost` tokens, or raise 429 with Retry-After."""
        wait = self.hit(name, key, cost)
        if wait > 0:
            raise rate_limited(wait)

    def prune(self) -> int:
        """Forget full buckets (they act exactly like new ones); returns how many went."""
        now = self._clock()
        full = []
        for key, bucket in self._buckets.items():
            refilled = bucket.tokens + max(0.0, now - bucket.updated) * bucket.rule.rate
            if refilled >= bucket.rule.capacity:
                full.append(key)
        for key in full:
            del self._buckets[key]
        if len(self._buckets) >= self._max_buckets:
            # Flooded with distinct keys: drop the least recently used tenth.
            oldest = sorted(self._buckets, key=lambda k: self._buckets[k].updated)
            for key in oldest[: max(1, len(oldest) // 10)]:
                del self._buckets[key]
        return len(full)

    def __len__(self) -> int:
        return len(self._buckets)
