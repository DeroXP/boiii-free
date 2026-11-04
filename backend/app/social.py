"""Friend graph and block queries shared by the routers and the presence hub."""

import uuid
from collections.abc import Iterable
from datetime import datetime

from sqlalchemy import and_, delete, or_, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models import Block, FriendRequest, Friendship, Invite, User
from app.schemas import username_lookup_key


def ordered_pair(a: uuid.UUID, b: uuid.UUID) -> tuple[uuid.UUID, uuid.UUID]:
    return (a, b) if a < b else (b, a)


async def find_user(db: AsyncSession, username: str) -> User | None:
    key = username_lookup_key(username)
    if key is None:
        return None
    return await db.scalar(select(User).where(User.username_lower == key))


async def usernames(db: AsyncSession, user_ids: Iterable[uuid.UUID]) -> dict[uuid.UUID, str]:
    ids = list(set(user_ids))
    if not ids:
        return {}
    rows = await db.execute(select(User.id, User.username).where(User.id.in_(ids)))
    return {user_id: name for user_id, name in rows}


async def friendship(db: AsyncSession, a: uuid.UUID, b: uuid.UUID) -> Friendship | None:
    low, high = ordered_pair(a, b)
    return await db.get(Friendship, (low, high))


async def are_friends(db: AsyncSession, a: uuid.UUID, b: uuid.UUID) -> bool:
    return await friendship(db, a, b) is not None


async def friends_since(db: AsyncSession, user_id: uuid.UUID) -> dict[uuid.UUID, datetime]:
    rows = await db.execute(
        select(Friendship.user_low, Friendship.user_high, Friendship.created_at).where(
            or_(Friendship.user_low == user_id, Friendship.user_high == user_id)
        )
    )
    return {(high if low == user_id else low): since for low, high, since in rows}


async def friend_ids(db: AsyncSession, user_id: uuid.UUID) -> set[uuid.UUID]:
    return set(await friends_since(db, user_id))


async def is_blocked_either_way(db: AsyncSession, a: uuid.UUID, b: uuid.UUID) -> bool:
    row = await db.scalar(
        select(Block.blocker_id)
        .where(
            or_(
                and_(Block.blocker_id == a, Block.blocked_id == b),
                and_(Block.blocker_id == b, Block.blocked_id == a),
            )
        )
        .limit(1)
    )
    return row is not None


async def block_partner_ids(db: AsyncSession, user_id: uuid.UUID) -> set[uuid.UUID]:
    """Everyone this user blocked, plus everyone who blocked this user."""
    rows = await db.execute(
        select(Block.blocker_id, Block.blocked_id).where(
            or_(Block.blocker_id == user_id, Block.blocked_id == user_id)
        )
    )
    return {blocked if blocker == user_id else blocker for blocker, blocked in rows}


async def add_friendship(
    db: AsyncSession, a: uuid.UUID, b: uuid.UUID, now: datetime
) -> Friendship:
    existing = await friendship(db, a, b)
    if existing is not None:
        return existing
    low, high = ordered_pair(a, b)
    row = Friendship(user_low=low, user_high=high, created_at=now)
    db.add(row)
    return row


async def remove_friendship(db: AsyncSession, a: uuid.UUID, b: uuid.UUID) -> bool:
    low, high = ordered_pair(a, b)
    result = await db.execute(
        delete(Friendship)
        .where(Friendship.user_low == low, Friendship.user_high == high)
        .execution_options(synchronize_session=False)
    )
    return bool(result.rowcount)


async def delete_requests_between(
    db: AsyncSession, a: uuid.UUID, b: uuid.UUID, *, keep_hidden: bool = False
) -> None:
    statement = delete(FriendRequest).where(
        or_(
            and_(FriendRequest.from_user_id == a, FriendRequest.to_user_id == b),
            and_(FriendRequest.from_user_id == b, FriendRequest.to_user_id == a),
        )
    )
    if keep_hidden:
        statement = statement.where(FriendRequest.hidden.is_(False))
    await db.execute(statement.execution_options(synchronize_session=False))


async def cut_ties(
    db: AsyncSession, a: uuid.UUID, b: uuid.UUID, *, keep_hidden: bool = False
) -> bool:
    """Remove the friendship, friend requests and invites between two users (a block does this).

    With `keep_hidden`, requests the recipient never sees stay. Returns whether they were friends.
    """
    were_friends = await remove_friendship(db, a, b)
    await delete_requests_between(db, a, b, keep_hidden=keep_hidden)
    await delete_invites_between(db, a, b)
    return were_friends


async def delete_invites_between(db: AsyncSession, a: uuid.UUID, b: uuid.UUID) -> None:
    await db.execute(
        delete(Invite)
        .where(
            or_(
                and_(Invite.from_user_id == a, Invite.to_user_id == b),
                and_(Invite.from_user_id == b, Invite.to_user_id == a),
            )
        )
        .execution_options(synchronize_session=False)
    )
