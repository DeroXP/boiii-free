"""Register, log in, refresh and log out."""

import logging
import unicodedata
import uuid

from fastapi import APIRouter, Request, Response
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.deps import DbDep, Services, ServicesDep
from app.errors import ApiError
from app.log import log_event
from app.models import DeletedUsername, RefreshToken, User, utcnow
from app.presence import CLOSE_UNAUTHORIZED
from app.schemas import AuthOut, LoginIn, MeOut, RefreshIn, RegisterIn, TokenOut, username_lookup_key
from app.tokens import (
    ACCESS_TOKEN_TTL_SECONDS,
    find_refresh_token,
    issue_refresh_token,
    revoke_family,
    rotate_refresh_token,
)

router = APIRouter(prefix="/v1/auth", tags=["auth"])


def _invalid_credentials() -> ApiError:
    # Same answer for an unknown username and a wrong password.
    return ApiError(401, "invalid_credentials", "Invalid username or password.")


def _invalid_refresh_token() -> ApiError:
    return ApiError(401, "invalid_token", "Refresh token is invalid or expired.")


async def _sign_in(db: AsyncSession, services: Services, user: User) -> AuthOut:
    refresh_token = await issue_refresh_token(db, user.id, utcnow())
    await db.commit()
    return AuthOut(
        user=MeOut.of(user),
        access_token=services.tokens.issue(user.id),
        refresh_token=refresh_token,
        expires_in=ACCESS_TOKEN_TTL_SECONDS,
    )


@router.post("/register", status_code=201, response_model=AuthOut)
async def register(
    body: RegisterIn, request: Request, db: DbDep, services: ServicesDep
) -> AuthOut:
    # Every attempt counts, a taken name too: otherwise this would answer "does this account
    # exist?" without limit.
    services.limiter.enforce("register", services.address_key(request.scope))
    username_lower = body.username.lower()
    taken = await db.scalar(select(User.id).where(User.username_lower == username_lower))
    held_until = await db.scalar(
        select(DeletedUsername.expires_at).where(DeletedUsername.username_lower == username_lower)
    )
    # A deleted account's name stays taken for 90 days, so nobody can pass as its former owner.
    if taken is not None or (held_until is not None and held_until > utcnow()):
        raise ApiError(409, "username_taken", "That username is taken.")
    user = User(
        id=uuid.uuid4(),
        username=body.username,
        username_lower=username_lower,
        password_hash=await services.passwords.hash(body.password),
        email=body.email,
        privacy_friend_requests="everyone",
        privacy_presence="friends",
        privacy_invites="friends",
        created_at=utcnow(),
    )
    db.add(user)
    try:
        await db.flush()
    except IntegrityError:  # lost a race for the same name
        await db.rollback()
        raise ApiError(409, "username_taken", "That username is taken.") from None
    result = await _sign_in(db, services, user)
    log_event("user_registered", user_id=str(user.id))
    return result


@router.post("/login", response_model=AuthOut)
async def login(body: LoginIn, request: Request, db: DbDep, services: ServicesDep) -> AuthOut:
    services.limiter.enforce("login_address", services.address_key(request.scope))
    lookup = username_lookup_key(body.username)
    # Failed attempts per username are limited too, from every address together. The attempt is
    # paid for before the hash and refunded if the password is right, so successful logins do
    # not count, and a burst of parallel guesses cannot all slip in while the first ones hash.
    username_bucket = lookup or unicodedata.normalize("NFKC", body.username).lower()
    services.limiter.enforce("login_username", username_bucket)

    user = None
    if lookup is not None:
        user = await db.scalar(select(User).where(User.username_lower == lookup))
    if user is None:
        await services.passwords.burn(body.password)
        raise _invalid_credentials()
    if not await services.passwords.verify(user.password_hash, body.password):
        log_event("login_failed", user_id=str(user.id))
        raise _invalid_credentials()
    services.limiter.refund("login_username", username_bucket)
    if services.passwords.needs_rehash(user.password_hash):
        user.password_hash = await services.passwords.hash(body.password)
        log_event("password_rehashed", user_id=str(user.id))
    result = await _sign_in(db, services, user)
    log_event("login", user_id=str(user.id))
    return result


async def _reuse_detected(
    db: AsyncSession, services: Services, user_id: uuid.UUID, family_id: uuid.UUID
) -> None:
    revoked = await revoke_family(db, family_id, utcnow())
    await db.commit()
    if revoked:
        # Whoever holds the live socket must show a current access token again.
        services.hub.kick(user_id, CLOSE_UNAUTHORIZED, "session revoked")
    log_event(
        "refresh_token_reuse",
        logging.WARNING,
        user_id=str(user_id),
        family_id=str(family_id),
        revoked=revoked,
    )


@router.post("/refresh", response_model=TokenOut)
async def refresh(body: RefreshIn, request: Request, db: DbDep, services: ServicesDep) -> TokenOut:
    now = utcnow()
    row: RefreshToken | None = await find_refresh_token(db, body.refresh_token)
    if row is None or row.revoked_at is not None or row.expires_at <= now:
        if row is not None and row.revoked_at is not None and row.replaced_by is not None:
            # An already-rotated token came back, so someone holds a copy: kill the family.
            await _reuse_detected(db, services, row.user_id, row.family_id)
        services.limiter.enforce("refresh", "address:" + services.address_key(request.scope))
        raise _invalid_refresh_token()
    user_id, family_id = row.user_id, row.family_id
    services.limiter.enforce("refresh", f"user:{user_id}")
    new_refresh_token = await rotate_refresh_token(db, row, now)
    if new_refresh_token is None:  # another request rotated this same token first
        await _reuse_detected(db, services, user_id, family_id)
        raise _invalid_refresh_token()
    await db.commit()
    return TokenOut(
        access_token=services.tokens.issue(user_id),
        refresh_token=new_refresh_token,
        expires_in=ACCESS_TOKEN_TTL_SECONDS,
    )


@router.post("/logout", status_code=204)
async def logout(body: RefreshIn, db: DbDep, services: ServicesDep) -> Response:
    row = await find_refresh_token(db, body.refresh_token)
    if row is not None:
        revoked = await revoke_family(db, row.family_id, utcnow())
        await db.commit()
        # Signed out means the live socket goes too. Only when this logout ended a session, so an
        # old, already revoked token cannot be replayed to keep kicking the user's socket.
        if revoked:
            services.hub.kick(row.user_id, CLOSE_UNAUTHORIZED, "signed out")
        log_event("logout", user_id=str(row.user_id))
    return Response(status_code=204)
