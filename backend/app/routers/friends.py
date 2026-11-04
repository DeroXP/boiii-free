"""Friends and friend requests.

Sending a request always answers 202 {"status": "sent"}, whether or not the user exists, has
blocked the sender or accepts requests, so the endpoint cannot be used to probe accounts. A request
to someone who blocked the sender or accepts no requests is still stored, but hidden: the sender
sees it pending in their outgoing list until it expires, and the recipient never sees it. So the
outgoing list does not give away blocks or privacy settings either. Otherwise a hidden request
acts like any other: sent again once the recipient would take it, it reaches them, and if the
recipient asks the sender themselves, the two become friends. (Whether a name exists at all is no
secret: registration has to say a name is taken.)
"""

import uuid
from datetime import timedelta

from fastapi import APIRouter, Response
from sqlalchemy import or_, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.deps import DbDep, ServicesDep, UserDep
from app.errors import not_found
from app.models import Block, FriendRequest, User, utcnow
from app.presence import PresenceHub, hide_presence_between, show_presence_between
from app.schemas import (
    FriendOut,
    FriendRequestOut,
    FriendRequestsOut,
    PresenceOut,
    StatusOut,
    UsernameIn,
)
from app.social import (
    add_friendship,
    are_friends,
    block_partner_ids,
    delete_invites_between,
    delete_requests_between,
    find_user,
    friends_since,
    is_blocked_either_way,
    remove_friendship,
    usernames,
)

router = APIRouter(prefix="/v1/friends", tags=["friends"])

FRIEND_REQUEST_TTL = timedelta(days=30)
SENT = StatusOut(status="sent")


def visible_presence(hub: PresenceHub, friend: User) -> PresenceOut | None:
    if friend.privacy_presence != "friends":
        return None
    presence = hub.presence(friend.id)
    return PresenceOut.model_validate(presence) if presence is not None else None


async def announce_friendship(db: AsyncSession, hub: PresenceHub, a: User, b: User) -> None:
    hub.send(a.id, {"type": "friend_added", "user_id": str(b.id), "username": b.username})
    hub.send(b.id, {"type": "friend_added", "user_id": str(a.id), "username": a.username})
    await show_presence_between(db, hub, a.id, b.id)


@router.get("", response_model=list[FriendOut])
async def list_friends(user: UserDep, db: DbDep, services: ServicesDep) -> list[FriendOut]:
    since = await friends_since(db, user.id)
    if not since:
        return []
    friends = (await db.scalars(select(User).where(User.id.in_(list(since))))).all()
    blocked = await block_partner_ids(db, user.id)
    return [
        FriendOut(
            user_id=friend.id,
            username=friend.username,
            since=since[friend.id],
            presence=None if friend.id in blocked else visible_presence(services.hub, friend),
        )
        for friend in sorted(friends, key=lambda f: f.username_lower)
    ]


@router.post("/requests", status_code=202, response_model=StatusOut)
async def send_request(
    body: UsernameIn, user: UserDep, db: DbDep, services: ServicesDep
) -> StatusOut:
    services.limiter.enforce("friend_request", str(user.id))
    target = await find_user(db, body.username)
    if target is None or target.id == user.id:
        return SENT
    # You blocked them, or you are friends already: nothing to hide from you, nothing to store.
    if await db.get(Block, (user.id, target.id)) is not None or await are_friends(
        db, user.id, target.id
    ):
        return SENT
    blocked_by_target = await db.get(Block, (target.id, user.id)) is not None
    hidden = blocked_by_target or target.privacy_friend_requests != "everyone"

    now = utcnow()
    reverse = await db.scalar(
        select(FriendRequest).where(
            FriendRequest.from_user_id == target.id, FriendRequest.to_user_id == user.id
        )
    )
    reverse_live = reverse is not None and reverse.expires_at > now
    if reverse is not None and reverse_live and not blocked_by_target:
        # They already asked us: sending one back is a yes. That holds for a request we never saw
        # (hidden) too. They did ask, and treating it differently would show them it was hidden.
        await add_friendship(db, user.id, target.id, now)
        await delete_requests_between(db, user.id, target.id)
        try:
            await db.commit()
        except IntegrityError:  # they accepted at the same moment; the friendship exists
            await db.rollback()
            return SENT
        await announce_friendship(db, services.hub, user, target)
        return SENT

    existing = await db.scalar(
        select(FriendRequest).where(
            FriendRequest.from_user_id == user.id, FriendRequest.to_user_id == target.id
        )
    )
    if existing is not None and existing.expires_at > now:
        if existing.hidden and not hidden:
            # Sent again now that the target would take it: from here on they see it. Its
            # timestamps stay as they were, so the sender cannot tell anything changed.
            existing.hidden = False
            await db.commit()
            services.hub.send(target.id, {"type": "friend_request"})
        return SENT
    if existing is not None:
        await db.delete(existing)
    if reverse is not None and not reverse_live:  # expired, so it no longer counts
        await db.delete(reverse)
    await db.flush()
    db.add(
        FriendRequest(
            id=uuid.uuid4(),
            from_user_id=user.id,
            to_user_id=target.id,
            created_at=now,
            expires_at=now + FRIEND_REQUEST_TTL,
            hidden=hidden,
        )
    )
    try:
        await db.commit()
    except IntegrityError:  # a concurrent duplicate won
        await db.rollback()
        return SENT
    if not hidden:
        services.hub.send(target.id, {"type": "friend_request"})
    return SENT


@router.get("/requests", response_model=FriendRequestsOut)
async def list_requests(user: UserDep, db: DbDep) -> FriendRequestsOut:
    now = utcnow()
    rows = (
        await db.scalars(
            select(FriendRequest)
            .where(
                or_(FriendRequest.from_user_id == user.id, FriendRequest.to_user_id == user.id),
                FriendRequest.expires_at > now,
            )
            .order_by(FriendRequest.created_at)
        )
    ).all()
    names = await usernames(
        db, [r.to_user_id if r.from_user_id == user.id else r.from_user_id for r in rows]
    )

    def out(request: FriendRequest, other: uuid.UUID) -> FriendRequestOut:
        return FriendRequestOut(
            id=request.id,
            user_id=other,
            username=names.get(other, ""),
            created_at=request.created_at,
            expires_at=request.expires_at,
        )

    return FriendRequestsOut(
        incoming=[
            out(r, r.from_user_id) for r in rows if r.to_user_id == user.id and not r.hidden
        ],
        # Hidden or not, an outgoing request looks the same to its sender.
        outgoing=[out(r, r.to_user_id) for r in rows if r.from_user_id == user.id],
    )


@router.post("/requests/{request_id}/accept", response_model=FriendOut)
async def accept_request(
    request_id: uuid.UUID, user: UserDep, db: DbDep, services: ServicesDep
) -> FriendOut:
    now = utcnow()
    request = await db.get(FriendRequest, request_id)
    if (
        request is None
        or request.to_user_id != user.id
        or request.hidden
        or request.expires_at <= now
    ):
        raise not_found("Friend request not found.")
    sender = await db.get(User, request.from_user_id)
    if sender is None or await is_blocked_either_way(db, user.id, sender.id):
        await db.delete(request)
        await db.commit()
        raise not_found("Friend request not found.")
    friendship = await add_friendship(db, user.id, sender.id, now)
    await delete_requests_between(db, user.id, sender.id)
    try:
        await db.commit()
    except IntegrityError:  # accepted twice at once; the other request made the friendship
        await db.rollback()
        raise not_found("Friend request not found.") from None
    await announce_friendship(db, services.hub, user, sender)
    return FriendOut(
        user_id=sender.id,
        username=sender.username,
        since=friendship.created_at,
        presence=visible_presence(services.hub, sender),
    )


@router.post("/requests/{request_id}/decline", status_code=204)
async def decline_request(request_id: uuid.UUID, user: UserDep, db: DbDep) -> Response:
    request = await db.get(FriendRequest, request_id)
    if request is None or request.to_user_id != user.id or request.hidden:
        raise not_found("Friend request not found.")
    await db.delete(request)
    await db.commit()
    return Response(status_code=204)


@router.delete("/requests/{request_id}", status_code=204)
async def cancel_request(request_id: uuid.UUID, user: UserDep, db: DbDep) -> Response:
    request = await db.get(FriendRequest, request_id)
    if request is None or request.from_user_id != user.id:
        raise not_found("Friend request not found.")
    await db.delete(request)
    await db.commit()
    return Response(status_code=204)


@router.delete("/{user_id}", status_code=204)
async def remove_friend(
    user_id: uuid.UUID, user: UserDep, db: DbDep, services: ServicesDep
) -> Response:
    removed = await remove_friendship(db, user.id, user_id)
    if removed:
        await delete_invites_between(db, user.id, user_id)
    await db.commit()
    if removed:
        await hide_presence_between(db, services.hub, user.id, user_id)
    return Response(status_code=204)
