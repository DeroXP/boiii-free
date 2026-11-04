"""Shared services and the FastAPI dependencies that hand them to routes."""

from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Annotated

from fastapi import Depends, Request
from sqlalchemy.ext.asyncio import AsyncEngine, AsyncSession, async_sessionmaker
from starlette.types import Scope

from app.config import RuntimeConfig, Tuning
from app.errors import ApiError
from app.models import User
from app.passwords import PasswordService
from app.presence import PresenceHub
from app.ratelimit import RateLimiter
from app.security import client_address, hashed_address
from app.tokens import AccessTokens, TokenError, issued_before_cutoff


@dataclass(eq=False)
class Services:
    config: RuntimeConfig
    tuning: Tuning
    engine: AsyncEngine
    sessionmaker: async_sessionmaker[AsyncSession]
    passwords: PasswordService
    tokens: AccessTokens
    limiter: RateLimiter
    hub: PresenceHub

    def address_key(self, scope: Scope) -> str:
        """Rate-limit key for the caller's address: an HMAC, never the address itself."""
        address = client_address(scope, self.config.trust_proxy, self.config.proxy_hops)
        return hashed_address(self.config.rate_limit_secret, address)


def get_services(request: Request) -> Services:
    return request.app.state.services


ServicesDep = Annotated[Services, Depends(get_services)]


async def get_db(services: ServicesDep) -> AsyncIterator[AsyncSession]:
    async with services.sessionmaker() as session:
        yield session


DbDep = Annotated[AsyncSession, Depends(get_db)]


def invalid_token() -> ApiError:
    return ApiError(
        401,
        "invalid_token",
        "Access token is invalid or expired.",
        headers={"WWW-Authenticate": 'Bearer error="invalid_token"'},
    )


async def current_user(request: Request, services: ServicesDep, db: DbDep) -> User:
    scheme, _, token = request.headers.get("authorization", "").partition(" ")
    token = token.strip()
    if scheme.lower() != "bearer" or not token:
        raise ApiError(
            401, "unauthorized", "Sign in first.", headers={"WWW-Authenticate": "Bearer"}
        )
    try:
        claims = services.tokens.decode(token)
    except TokenError:
        raise invalid_token() from None
    user = await db.get(User, claims.user_id)
    # Gone: the account was deleted. Cut off: the password changed after the token was issued.
    if user is None or issued_before_cutoff(claims.issued_at, user.tokens_valid_after):
        raise invalid_token()
    return user


UserDep = Annotated[User, Depends(current_user)]
