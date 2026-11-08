"""The signed-in account: profile, privacy, password, data export and deletion."""

import asyncio
import uuid
from datetime import UTC, datetime, time, timedelta
from typing import Any

from fastapi import APIRouter, Request, Response
from sqlalchemy import delete, or_, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.deps import DbDep, Services, ServicesDep, UserDep, invalid_token
from app.errors import ApiError
from app.log import log_event
from app.models import (
    Block,
    DeletedUsername,
    FriendRequest,
    Friendship,
    Invite,
    Pack,
    RefreshToken,
    Report,
    User,
    iso,
    utcnow,
)
from app.presence import (
    CLOSE_ACCOUNT_DELETED,
    CLOSE_UNAUTHORIZED,
    broadcast_presence,
    offline_event,
    presence_audience,
)
from app.schemas import (
    AddressOut,
    MeOut,
    PasswordChangeIn,
    PasswordConfirmIn,
    PrivacyPatch,
    password_matches_username,
)
from app.security import public_address
from app.social import usernames
from app.tokens import lock_account, revoke_all_for_user

router = APIRouter(prefix="/v1/me", tags=["me"])

# A deleted account's name stays taken this long (DESIGN.md 10.2).
DELETED_USERNAME_HOLD = timedelta(days=90)


def _invalid_password() -> ApiError:
    return ApiError(403, "invalid_password", "Password is incorrect.")


async def _check_password(
    db: AsyncSession, services: Services, user: User, password: str
) -> None:
    """Wrong passwords here are rate limited per account, like logins.

    The check is paid for before the hash and given back unless the password is wrong, so a burst
    of parallel guesses (say, with a stolen access token) cannot all slip in while the first hash,
    and a server error does not count. The read that loaded the account ends first, so no
    database connection waits for the hash. Callers hold `services.passwords.admit()`.
    """
    key = str(user.id)
    services.limiter.enforce("password_check", key)
    counted = False
    try:
        await db.commit()
        if not await services.passwords.verify(user.password_hash, password):
            counted = True
            raise _invalid_password()
    finally:
        if not counted:
            services.limiter.refund("password_check", key)


async def _lock_unchanged(db: AsyncSession, user_id: uuid.UUID, checked_hash: str) -> None:
    """Lock the account (app.tokens.lock_account) and check its password is still the one checked.

    A password change that committed while this request was hashing must win: this one answers
    403 rather than overwrite it, or delete an account whose password is no longer the one given.
    """
    account = await lock_account(db, user_id)
    if account is None:
        await db.rollback()
        raise invalid_token()  # deleted meanwhile
    if account.password_hash != checked_hash:
        await db.rollback()
        raise _invalid_password()


@router.get("", response_model=MeOut)
async def get_me(user: UserDep) -> MeOut:
    return MeOut.of(user)


@router.get("/address", response_model=AddressOut)
async def get_address(request: Request, user: UserDep, services: ServicesDep) -> AddressOut:
    """The caller's own public IP address, which a host's launcher puts in a game invite when the
    router has no UPnP to tell it (DESIGN.md 10.3). Signed in only, so this is no open address
    echo; the address is neither stored nor logged."""
    services.limiter.enforce("own_address", str(user.id))
    config = services.config
    address = public_address(request.scope, config.trust_proxy, config.proxy_hops)
    if address is None:
        raise ApiError(503, "address_unknown", "The server cannot tell your public address.")
    return AddressOut(address=address)


@router.patch("/privacy", response_model=MeOut)
async def update_privacy(
    body: PrivacyPatch, user: UserDep, db: DbDep, services: ServicesDep
) -> MeOut:
    was_visible = user.privacy_presence == "friends"
    if body.friend_requests is not None:
        user.privacy_friend_requests = body.friend_requests
    if body.presence is not None:
        user.privacy_presence = body.presence
    if body.invites is not None:
        user.privacy_invites = body.invites
    now_visible = user.privacy_presence == "friends"

    hidden_from = []
    if was_visible and not now_visible and services.hub.is_online(user.id):
        hidden_from = await presence_audience(db, services.hub, user.id, respect_privacy=False)
    await db.commit()
    if hidden_from:
        services.hub.send_many(hidden_from, offline_event(user.id))
    elif now_visible and not was_visible:
        await broadcast_presence(db, services.hub, user.id)
    return MeOut.of(user)


@router.post("/password", status_code=204)
async def change_password(
    body: PasswordChangeIn, user: UserDep, db: DbDep, services: ServicesDep
) -> Response:
    with services.passwords.admit():
        await _check_password(db, services, user, body.current_password)
        if password_matches_username(body.new_password, user.username):
            raise ApiError(
                422,
                "invalid_request",
                "new_password: Password must not be the same as the username.",
            )
        new_hash = await services.passwords.hash(body.new_password)
    await _lock_unchanged(db, user.id, user.password_hash)
    user.password_hash = new_hash
    now = utcnow()
    await revoke_all_for_user(db, user.id, now)
    # Access tokens issued until now stop working too, over HTTP and for opening a WebSocket.
    # Their `iat` has whole seconds, so the cut-off is the start of the next second: that also
    # catches a token minted earlier in this same second (say, by a thief refreshing in a loop).
    cutoff = now.replace(microsecond=0) + timedelta(seconds=1)
    user.tokens_valid_after = cutoff
    await db.commit()
    # Every session must sign in again; that includes the live WebSocket.
    services.hub.kick(user.id, CLOSE_UNAUTHORIZED, "password changed")
    log_event("password_changed", user_id=str(user.id))
    # Answer once that second has begun (by the wall clock that `iat` uses; a timer can fire a
    # little early), so the sign-in that follows gets a token past the cut-off.
    while (remaining := (cutoff - utcnow()).total_seconds()) > 0:
        await asyncio.sleep(remaining)
    return Response(status_code=204)


@router.get("/export")
async def export_me(user: UserDep, db: DbDep) -> dict[str, Any]:
    """Every row about this user. No hashes, and nothing private about anyone else.

    Reports other people filed about this user are listed by reason, status and day only: who
    filed them and what they wrote stay out, to protect the reporters.

    Every list is in a fixed order (by creation). Without one, PostgreSQL hands rows back as they
    lie on disk, and a row moves when it is updated: that would show the sender which of their
    friend requests had been hidden, once sending one again made it visible.
    """
    me = user.id
    sessions = (
        await db.scalars(
            select(RefreshToken)
            .where(RefreshToken.user_id == me)
            .order_by(RefreshToken.created_at, RefreshToken.id)
        )
    ).all()
    friendships = (
        await db.scalars(
            select(Friendship)
            .where(or_(Friendship.user_low == me, Friendship.user_high == me))
            .order_by(Friendship.created_at, Friendship.user_low, Friendship.user_high)
        )
    ).all()
    requests = (
        await db.scalars(
            select(FriendRequest)
            .where(or_(FriendRequest.from_user_id == me, FriendRequest.to_user_id == me))
            .order_by(FriendRequest.created_at, FriendRequest.id)
        )
    ).all()
    blocks = (
        await db.scalars(
            select(Block)
            .where(Block.blocker_id == me)
            .order_by(Block.created_at, Block.blocked_id)
        )
    ).all()
    reports = (
        await db.scalars(
            select(Report).where(Report.reporter_id == me).order_by(Report.created_at, Report.id)
        )
    ).all()
    reports_about = (
        await db.scalars(
            select(Report).where(Report.reported_id == me).order_by(Report.created_at, Report.id)
        )
    ).all()
    invites = (
        await db.scalars(
            select(Invite)
            .where(or_(Invite.from_user_id == me, Invite.to_user_id == me))
            .order_by(Invite.created_at, Invite.id)
        )
    ).all()
    packs = (
        await db.scalars(
            select(Pack).where(Pack.owner_id == me).order_by(Pack.created_at, Pack.id)
        )
    ).all()

    other_ids = {f.user_high if f.user_low == me else f.user_low for f in friendships}
    other_ids |= {r.to_user_id if r.from_user_id == me else r.from_user_id for r in requests}
    other_ids |= {b.blocked_id for b in blocks}
    other_ids |= {r.reported_id for r in reports if r.reported_id is not None}
    other_ids |= {i.to_user_id if i.from_user_id == me else i.from_user_id for i in invites}
    names = await usernames(db, other_ids)

    def invite_row(invite: Invite, other_key: str, other_id: Any) -> dict[str, Any]:
        return {
            "id": str(invite.id),
            other_key + "_user_id": str(other_id),
            other_key + "_username": names.get(other_id),
            "session": invite.session,
            "pack_code": invite.pack_code,
            "status": invite.status,
            "created_at": iso(invite.created_at),
            "expires_at": iso(invite.expires_at),
        }

    return {
        "exported_at": iso(utcnow()),
        "user": {
            **MeOut.of(user).model_dump(mode="json"),
            "tokens_valid_after": iso(user.tokens_valid_after),
        },
        "sessions": [
            {
                "id": str(token.id),
                "family_id": str(token.family_id),
                "created_at": iso(token.created_at),
                "expires_at": iso(token.expires_at),
                "revoked_at": iso(token.revoked_at),
            }
            for token in sessions
        ],
        "friends": [
            {
                "user_id": str(other),
                "username": names.get(other),
                "since": iso(f.created_at),
            }
            for f in friendships
            for other in [f.user_high if f.user_low == me else f.user_low]
        ],
        "friend_requests": {
            "incoming": [
                {
                    "id": str(r.id),
                    "from_user_id": str(r.from_user_id),
                    "from_username": names.get(r.from_user_id),
                    "created_at": iso(r.created_at),
                    "expires_at": iso(r.expires_at),
                    # Sent while you had blocked the sender or accepted no requests, so never
                    # shown to you. Outgoing requests carry no such flag: it would tell the
                    # sender they are blocked.
                    "hidden": r.hidden,
                }
                for r in requests
                if r.to_user_id == me
            ],
            "outgoing": [
                {
                    "id": str(r.id),
                    "to_user_id": str(r.to_user_id),
                    "to_username": names.get(r.to_user_id),
                    "created_at": iso(r.created_at),
                    "expires_at": iso(r.expires_at),
                }
                for r in requests
                if r.from_user_id == me
            ],
        },
        "blocks": [
            {
                "user_id": str(b.blocked_id),
                "username": names.get(b.blocked_id),
                "created_at": iso(b.created_at),
            }
            for b in blocks
        ],
        "reports_filed": [
            {
                "id": str(r.id),
                "reported_user_id": str(r.reported_id) if r.reported_id else None,
                "reported_username": names.get(r.reported_id) if r.reported_id else None,
                "reason": r.reason,
                "details": r.details,
                "status": r.status,
                "created_at": iso(r.created_at),
            }
            for r in reports
        ],
        # The day only: an exact time could point at whoever filed it.
        "reports_about_you": [
            {
                "reason": r.reason,
                "status": r.status,
                "created_on": r.created_at.date().isoformat(),
            }
            for r in reports_about
        ],
        "invites": {
            "received": [invite_row(i, "from", i.from_user_id) for i in invites if i.to_user_id == me],
            "sent": [invite_row(i, "to", i.to_user_id) for i in invites if i.from_user_id == me],
        },
        "packs": [
            {
                "code": p.code,
                "pack": p.data,
                "created_at": iso(p.created_at),
                "expires_at": iso(p.expires_at),
            }
            for p in packs
        ],
    }


@router.delete("", status_code=204)
async def delete_me(
    body: PasswordConfirmIn, user: UserDep, db: DbDep, services: ServicesDep
) -> Response:
    """Hard delete. Foreign keys cascade; reports about or by this user lose the link.

    The name stays taken for 90 days (kept with no link to the account), so nobody can register
    it at once and pass as the person who left.
    """
    with services.passwords.admit():
        await _check_password(db, services, user, body.password)
    user_id, checked_hash = user.id, user.password_hash
    await _hold_username(db, user.username_lower)
    await _lock_unchanged(db, user_id, checked_hash)
    watching = await presence_audience(db, services.hub, user_id)
    await db.execute(
        delete(User).where(User.id == user_id).execution_options(synchronize_session=False)
    )
    await db.commit()
    services.hub.kick(user_id, CLOSE_ACCOUNT_DELETED, "account deleted")
    services.hub.send_many(watching, offline_event(user_id))
    # No user id: with one, this line would tie the held name back to the account's other logs.
    log_event("account_deleted")
    return Response(status_code=204)


async def _hold_username(db: AsyncSession, username_lower: str) -> None:
    """Hold a name for 90 days, in a transaction of its own, before its account is deleted.

    So the name is never free for a moment, and the hold shares nothing with the deletion, not
    even a transaction id (PostgreSQL's xmin) that would match the reports it unlinks. Its expiry
    is the midnight (UTC) after the 90 days: an exact time would match the deletion's log line.
    """
    held_until = datetime.combine(
        (utcnow() + DELETED_USERNAME_HOLD).date() + timedelta(days=1), time.min, tzinfo=UTC
    )
    hold = await db.get(DeletedUsername, username_lower)
    if hold is None:
        db.add(DeletedUsername(username_lower=username_lower, expires_at=held_until))
    else:  # an expired hold the cleanup task has not removed yet
        hold.expires_at = held_until
    try:
        await db.commit()
    except IntegrityError:  # deleted twice at once; the other request holds it
        await db.rollback()
