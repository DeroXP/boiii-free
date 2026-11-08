"""In-memory token-bucket rate limits.

State lives in this process only, which is why the service runs as a single replica. Keys are user
ids, normalized usernames or HMACs of client addresses (see app.security); raw addresses never
reach this module.
"""

import math
import time
from collections.abc import Callable, Mapping, Sequence
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
MIB = 1024 * 1024

DEFAULT_RULES: Mapping[str, Rule] = {
    "register": Rule(3, HOUR),  # every attempt (taken names too), per hashed address
    "login_address": Rule(10, 15 * 60),  # every login attempt, per hashed address
    # Failed logins (paid before the hash, refunded unless the password was wrong): per username
    # and hashed address, and per username from every address together.
    "login_username_address": Rule(5, 15 * 60),
    "login_username": Rule(50, 15 * 60),
    "refresh": Rule(30, 60),  # per user (per hashed address for unusable tokens)
    "friend_request": Rule(20, DAY),  # per user
    "invite": Rule(30, HOUR),  # per user
    "invite_recipient": Rule(1, 30),  # per sender and recipient
    "own_address": Rule(30, HOUR),  # GET /v1/me/address, per user
    "report": Rule(10, DAY),  # per user
    "pack_create": Rule(20, DAY),  # per user
    "pack_fetch": Rule(120, 60),  # per hashed address (codes are public, this stops scraping)
    "password_check": Rule(5, 15 * 60),  # wrong passwords on /v1/me, per user (refunded if right)
    "ws_message": Rule(10, 1),  # per user, whichever socket the messages come over
    # Per user, and per hashed address for sockets that do not sign in (refunded when they do).
    "ws_connect": Rule(10, 60),
    # Crash reports: per install id (a launcher sends its outbox of up to 20 at start); per hashed
    # address (several testers can share one), counted before the body is read and then by the
    # decoded body's bytes; and every stored report together, by count and by bytes stored. What
    # the database holds in total is capped apart from these (XERCES_CRASH_STORAGE_MB).
    "crash_install": Rule(20, HOUR),
    "crash_address": Rule(60, HOUR),
    "crash_address_bytes": Rule(64 * MIB, HOUR),
    "crash_all": Rule(2000, DAY),
    "crash_bytes": Rule(256 * MIB, DAY),
    "crash_delete": Rule(10, HOUR),  # per hashed address
    "admin_auth": Rule(10, 15 * 60),  # wrong admin tokens, per hashed address (refunded if right)
}


# One spend in RateLimiter.enforce_all: (rule, key), or (rule, key, cost).
Charge = tuple[str, str] | tuple[str, str, float]


def rate_limited(wait_seconds: float) -> ApiError:
    retry_after = str(max(1, math.ceil(wait_seconds)))
    return ApiError(
        429,
        "rate_limited",
        "Too many requests. Try again later.",
        headers={"Retry-After": retry_after},
    )


class TokenBucket:
    """A single bucket: one key of one rule."""

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

    def full_at(self, now: float) -> bool:
        return self.tokens + max(0.0, now - self.updated) * self.rule.rate >= self.rule.capacity


class RateLimiter:
    """Buckets per rule and key, each rule holding at most `max_buckets` of them (about 23 MB).

    A full bucket acts exactly like a new one, so those are what gets forgotten. A bucket that is
    still limiting someone never is: when a rule has no room left, its new keys are refused until
    buckets refill. Forgetting one instead would let a flood of fresh keys (cheap with IPv6
    addresses) reset the bucket of the account under attack.
    """

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
        self._stores: dict[str, dict[str, TokenBucket]] = {}
        self._pruned_at: dict[str, float] = {}

    def hit(self, name: str, key: str, cost: float = 1.0) -> float:
        """Spend `cost` tokens. Returns 0 when allowed, else the seconds until it would be."""
        rule = self.rules[name]
        store = self._stores.setdefault(name, {})
        bucket = store.get(key)
        if bucket is None:
            if len(store) >= self._max_buckets and not self._make_room(name, store):
                return rule.period / rule.capacity  # until one of them could have refilled
            bucket = store[key] = TokenBucket(rule, self._clock)
        if bucket.take(cost):
            return 0.0
        return (cost - bucket.tokens) / rule.rate

    def refund(self, name: str, key: str, amount: float = 1.0) -> None:
        """Give back what `hit` spent, for attempts that turn out not to count (a right password).

        Spend first and refund later, never check first and spend later: requests that arrive
        while an earlier one is still awaiting (a password hash) would all pass the check.
        """
        bucket = self._stores.get(name, {}).get(key)
        if bucket is not None:
            bucket.tokens = min(bucket.rule.capacity, bucket.tokens + amount)

    def enforce(self, name: str, key: str, cost: float = 1.0) -> None:
        """Spend `cost` tokens, or raise 429 with Retry-After."""
        wait = self.hit(name, key, cost)
        if wait > 0:
            raise rate_limited(wait)

    def enforce_all(self, buckets: Sequence[Charge]) -> None:
        """Spend from each (rule, key) or (rule, key, cost), or from none of them: raise 429 if
        any is short. The cost is 1 unless given."""
        for index, (name, key, *cost) in enumerate(buckets):
            wait = self.hit(name, key, *cost)
            if wait > 0:
                for spent in buckets[:index]:
                    self.refund(*spent)
                raise rate_limited(wait)

    def refund_all(self, buckets: Sequence[Charge]) -> None:
        for bucket in buckets:
            self.refund(*bucket)

    def _make_room(self, name: str, store: dict[str, TokenBucket]) -> bool:
        # Scanning every bucket of a rule costs milliseconds, so a flood of new keys gets at most
        # one scan a second; in between they are refused.
        now = self._clock()
        if now - self._pruned_at.get(name, -math.inf) >= 1.0:
            self._pruned_at[name] = now
            self._drop_full(store, now)
        return len(store) < self._max_buckets

    @staticmethod
    def _drop_full(store: dict[str, TokenBucket], now: float) -> int:
        full = [key for key, bucket in store.items() if bucket.full_at(now)]
        for key in full:
            del store[key]
        return len(full)

    def prune(self) -> int:
        """Forget full buckets (they act exactly like new ones); returns how many went."""
        now = self._clock()
        return sum(self._drop_full(store, now) for store in self._stores.values())

    def __len__(self) -> int:
        return sum(len(store) for store in self._stores.values())
