"""Register, log in, refresh and log out."""

import logging
import uuid
from datetime import timedelta

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
    lock_account,
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
    # exist?" without limit. One turned away because the server is busy does not count.
    services.passwords.ensure_room()
    services.limiter.enforce("register", services.address_key(request.scope))
    username_lower = body.username.lower()
    with services.passwords.admit():
        taken = await db.scalar(select(User.id).where(User.username_lower == username_lower))
        held_until = await db.scalar(
            select(DeletedUsername.expires_at).where(
                DeletedUsername.username_lower == username_lower
            )
        )
        # A deleted account's name stays taken for 90 days, so nobody can pass as its former
        # owner.
        if taken is not None or (held_until is not None and held_until > utcnow()):
            raise ApiError(409, "username_taken", "That username is taken.")
        # No database connection waits for the hash: the pool is shared with every other route.
        await db.commit()
        password_hash = await services.passwords.hash(body.password)
    user = User(
        id=uuid.uuid4(),
        username=body.username,
        username_lower=username_lower,
        password_hash=password_hash,
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
    address = services.address_key(request.scope)
    services.passwords.ensure_room()  # turned away busy, an attempt costs the address nothing
    services.limiter.enforce("login_address", address)
    lookup = username_lookup_key(body.username)
    # Failed logins are limited per username and address, and per username from every address
    # together (README: Rate limits). An attempt pays before the hash, so a burst of parallel
    # guesses cannot all slip in while the first ones hash, and is paid back unless it ends in a
    # wrong password: a right one, a server error or a full hash queue do not count. A name that
    # no account can have has nothing to protect (and would make an unbounded key).
    attempt = (
        []
        if lookup is None
        else [("login_username_address", f"{lookup}:{address}"), ("login_username", lookup)]
    )
    services.limiter.enforce_all(attempt)
    counted = False
    new_hash = None
    try:
        with services.passwords.admit():
            user = None
            if lookup is not None:
                user = await db.scalar(select(User).where(User.username_lower == lookup))
            # No database connection waits for the hash: the pool is shared with every route.
            await db.commit()
            if user is None:
                await services.passwords.burn(body.password)
                counted = True
                raise _invalid_credentials()
            checked_hash = user.password_hash
            if not await services.passwords.verify(checked_hash, body.password):
                counted = True
                log_event("login_failed", user_id=str(user.id))
                raise _invalid_credentials()
            if services.passwords.needs_rehash(checked_hash):
                new_hash = await services.passwords.hash(body.password)
        account = await lock_account(db, user.id)
        if account is None or account.password_hash != checked_hash:
            # The password changed (or the account was deleted) while this one was checked: it is
            # no longer the password, and the session it would get has already been revoked.
            await db.rollback()
            counted = True
            raise _invalid_credentials()
        if new_hash is not None:
            user.password_hash = new_hash  # under the lock, so it never overwrites a change
        result = await _sign_in(db, services, user)
    finally:
        if not counted:
            services.limiter.refund_all(attempt)
    if new_hash is not None:
        log_event("password_rehashed", user_id=str(user.id))
    log_event("login", user_id=str(user.id))
    return result


async def _reuse_detected(
    db: AsyncSession, services: Services, user_id: uuid.UUID, family_id: uuid.UUID
) -> None:
    # Under the lock, so a refresh of this family in flight finishes first and its token dies too.
    await lock_account(db, user_id)
    revoked = await revoke_family(db, family_id, utcnow())
    await db.commit()
    if revoked:
        # Once per family: a token replayed again finds nothing left to revoke, and stays quiet.
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
        # Limited before anything else, so replaying a spent token cannot buy unlimited work.
        services.limiter.enforce("refresh", "address:" + services.address_key(request.scope))
        if row is not None and row.revoked_at is not None and row.replaced_by is not None:
            # An already-rotated token came back, so someone holds a copy: kill the family.
            await _reuse_detected(db, services, row.user_id, row.family_id)
        raise _invalid_refresh_token()
    user_id, family_id = row.user_id, row.family_id
    services.limiter.enforce("refresh", f"user:{user_id}")
    account = await lock_account(db, user_id)
    if account is None:  # deleted meanwhile
        raise _invalid_refresh_token()
    cutoff = account.tokens_valid_after
    if cutoff is not None and row.created_at < cutoff - timedelta(seconds=1):
        # Older than the last password change, which revoked every token. Should one have
        # escaped that, it still ends here, with its family.
        await revoke_family(db, family_id, now)
        await db.commit()
        raise _invalid_refresh_token()
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
        # Under the lock, so a refresh of this family in flight finishes first and is revoked too.
        await lock_account(db, row.user_id)
        revoked = await revoke_family(db, row.family_id, utcnow())
        await db.commit()
        # Signed out means the live socket goes too. Only when this logout ended a session, so an
        # old, already revoked token cannot be replayed to keep kicking the user's socket.
        if revoked:
            services.hub.kick(row.user_id, CLOSE_UNAUTHORIZED, "signed out")
        log_event("logout", user_id=str(row.user_id))
    return Response(status_code=204)
