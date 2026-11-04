"""Access tokens (EdDSA JWT, 10 minutes) and refresh tokens (random, stored hashed, rotated).

Refresh tokens follow RFC 9700: every refresh swaps the token for a new one in the same family, and
presenting a token that was already swapped revokes the whole family.
"""

import base64
import hashlib
import re
import secrets
import time
import uuid
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import datetime, timedelta

import jwt
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)
from sqlalchemy import select, update
from sqlalchemy.ext.asyncio import AsyncSession

from app.models import RefreshToken

ISSUER = "xerces"
AUDIENCE = "xerces-client"
ALGORITHM = "EdDSA"
ACCESS_TOKEN_TTL_SECONDS = 600
REFRESH_TOKEN_TTL = timedelta(days=30)
MAX_ACCESS_TOKEN_CHARS = 2048
_REQUIRED_CLAIMS = ["iss", "aud", "typ", "sub", "iat", "exp", "jti"]
# 32 random bytes in unpadded base64url.
_REFRESH_TOKEN_SHAPE = re.compile(r"[A-Za-z0-9_-]{43}")


class TokenError(Exception):
    """Malformed, expired, wrong type, or signed by a key this server does not know."""


@dataclass(frozen=True)
class AccessClaims:
    user_id: uuid.UUID
    issued_at: int  # iat, seconds since the epoch
    expires_at: int  # exp, seconds since the epoch


def issued_before_cutoff(issued_at: int, cutoff: datetime | None) -> bool:
    """True when a token was issued before the user's cut-off, set on a password change to the
    whole second after it (`iat` has whole seconds; see app.routers.me.change_password)."""
    return cutoff is not None and issued_at < int(cutoff.timestamp())


class AccessTokens:
    def __init__(
        self,
        signing_key: Ed25519PrivateKey,
        key_id: str,
        verify_keys: Mapping[str, Ed25519PublicKey],
    ) -> None:
        self._signing_key = signing_key
        self._key_id = key_id
        self._verify_keys = dict(verify_keys)

    def issue(self, user_id: uuid.UUID, *, now: float | None = None) -> str:
        issued_at = int(time.time() if now is None else now)
        claims = {
            "iss": ISSUER,
            "aud": AUDIENCE,
            "typ": "access",
            "sub": str(user_id),
            "iat": issued_at,
            "exp": issued_at + ACCESS_TOKEN_TTL_SECONDS,
            "jti": secrets.token_urlsafe(16),
        }
        return jwt.encode(claims, self._signing_key, algorithm=ALGORITHM, headers={"kid": self._key_id})

    def verify(self, token: str) -> uuid.UUID:
        """Return the user id in a valid access token, else raise TokenError."""
        return self.decode(token).user_id

    def decode(self, token: str) -> AccessClaims:
        """Return the claims of a valid access token, else raise TokenError.

        This checks the token alone; callers also check the user's cut-off (`issued_before_cutoff`).
        """
        # A JWT is ASCII. Anything else (such as a lone surrogate from a JSON frame) would make
        # PyJWT raise UnicodeEncodeError instead of a token error.
        if not token or len(token) > MAX_ACCESS_TOKEN_CHARS or not token.isascii():
            raise TokenError("bad token")
        try:
            header = jwt.get_unverified_header(token)
        except jwt.PyJWTError as exc:
            raise TokenError("bad header") from exc
        kid = header.get("kid")
        key = self._verify_keys.get(kid) if isinstance(kid, str) else None
        if key is None:
            raise TokenError("unknown key id")
        try:
            claims = jwt.decode(
                token,
                key,
                algorithms=[ALGORITHM],
                audience=AUDIENCE,
                issuer=ISSUER,
                options={"require": _REQUIRED_CLAIMS},
            )
        except jwt.PyJWTError as exc:
            raise TokenError("invalid token") from exc
        if claims.get("typ") != "access":
            raise TokenError("wrong token type")
        try:
            user_id = uuid.UUID(claims["sub"])
        except (ValueError, TypeError, AttributeError) as exc:
            raise TokenError("bad subject") from exc
        issued_at, expires_at = claims["iat"], claims["exp"]
        if not all(type(value) in (int, float) for value in (issued_at, expires_at)):
            raise TokenError("bad timestamps")
        return AccessClaims(user_id=user_id, issued_at=int(issued_at), expires_at=int(expires_at))


def hash_refresh_token(raw: str) -> str:
    return hashlib.sha256(raw.encode("ascii")).hexdigest()


def new_refresh_token() -> tuple[str, str]:
    """Return (token for the client, hash for the database)."""
    raw = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode("ascii")
    return raw, hash_refresh_token(raw)


async def issue_refresh_token(
    db: AsyncSession,
    user_id: uuid.UUID,
    now: datetime,
    *,
    family_id: uuid.UUID | None = None,
) -> str:
    raw, token_hash = new_refresh_token()
    db.add(
        RefreshToken(
            id=uuid.uuid4(),
            user_id=user_id,
            token_hash=token_hash,
            family_id=family_id or uuid.uuid4(),
            created_at=now,
            expires_at=now + REFRESH_TOKEN_TTL,
        )
    )
    return raw


async def find_refresh_token(db: AsyncSession, raw: str) -> RefreshToken | None:
    if not _REFRESH_TOKEN_SHAPE.fullmatch(raw):
        return None
    return await db.scalar(
        select(RefreshToken).where(RefreshToken.token_hash == hash_refresh_token(raw))
    )


async def rotate_refresh_token(db: AsyncSession, row: RefreshToken, now: datetime) -> str | None:
    """Swap an active token for a new one in its family.

    Returns None when the row was rotated or revoked concurrently, which the caller treats as
    reuse. The conditional UPDATE makes the swap atomic on both PostgreSQL and SQLite.
    """
    raw, token_hash = new_refresh_token()
    new_id = uuid.uuid4()
    result = await db.execute(
        update(RefreshToken)
        .where(RefreshToken.id == row.id, RefreshToken.revoked_at.is_(None))
        .values(revoked_at=now, replaced_by=new_id)
        .execution_options(synchronize_session=False)
    )
    if result.rowcount != 1:
        return None
    db.add(
        RefreshToken(
            id=new_id,
            user_id=row.user_id,
            token_hash=token_hash,
            family_id=row.family_id,
            created_at=now,
            expires_at=now + REFRESH_TOKEN_TTL,
        )
    )
    return raw


async def revoke_family(db: AsyncSession, family_id: uuid.UUID, now: datetime) -> int:
    result = await db.execute(
        update(RefreshToken)
        .where(RefreshToken.family_id == family_id, RefreshToken.revoked_at.is_(None))
        .values(revoked_at=now)
        .execution_options(synchronize_session=False)
    )
    return result.rowcount or 0


async def revoke_all_for_user(db: AsyncSession, user_id: uuid.UUID, now: datetime) -> int:
    result = await db.execute(
        update(RefreshToken)
        .where(RefreshToken.user_id == user_id, RefreshToken.revoked_at.is_(None))
        .values(revoked_at=now)
        .execution_options(synchronize_session=False)
    )
    return result.rowcount or 0
