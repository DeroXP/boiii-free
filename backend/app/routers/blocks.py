"""Blocks. A block removes the friendship, pending requests and invites in both directions, and
afterwards neither side can see the other's presence or reach the other."""

import uuid

from fastapi import APIRouter, Response
from sqlalchemy import delete, select
from sqlalchemy.exc import IntegrityError

from app.deps import DbDep, ServicesDep, UserDep
from app.errors import ApiError
from app.log import log_event
from app.models import Block, utcnow
from app.presence import hide_presence_between
from app.schemas import BlockOut, UsernameIn
from app.social import cut_ties, find_user, usernames

router = APIRouter(prefix="/v1/blocks", tags=["blocks"])


@router.get("", response_model=list[BlockOut])
async def list_blocks(user: UserDep, db: DbDep) -> list[BlockOut]:
    rows = (
        await db.scalars(
            select(Block).where(Block.blocker_id == user.id).order_by(Block.created_at)
        )
    ).all()
    names = await usernames(db, [row.blocked_id for row in rows])
    return [
        BlockOut(user_id=row.blocked_id, username=names.get(row.blocked_id, ""), created_at=row.created_at)
        for row in rows
    ]


@router.post("", status_code=204)
async def block_user(
    body: UsernameIn, user: UserDep, db: DbDep, services: ServicesDep
) -> Response:
    target = await find_user(db, body.username)
    if target is None:
        # Same answer as a real block: this endpoint does not reveal which names exist.
        return Response(status_code=204)
    if target.id == user.id:
        raise ApiError(422, "invalid_request", "You cannot block yourself.")
    if await db.get(Block, (user.id, target.id)) is None:
        db.add(Block(blocker_id=user.id, blocked_id=target.id, created_at=utcnow()))
    were_friends = await cut_ties(db, user.id, target.id)
    try:
        await db.commit()
    except IntegrityError:  # blocked twice at once; the other request did the work
        await db.rollback()
        were_friends = False
    # A request, invite or acceptance that checked for a block before this one committed may still
    # be on its way. From here on each reads again before it pushes anything (app.presence), and
    # whatever it wrote after the cut above is cut now. A request kept hidden from the blocker
    # stays: it is what anyone sending one now gets.
    services.hub.note_block()
    were_friends = await cut_ties(db, user.id, target.id, keep_hidden=True) or were_friends
    await db.commit()
    # Only friends saw each other's presence, so only they are told it went away. Telling anyone
    # else would let a block (and unblock) reveal whether a stranger is online.
    if were_friends:
        await hide_presence_between(db, services.hub, user.id, target.id)
    log_event("user_blocked", user_id=str(user.id))
    return Response(status_code=204)


@router.delete("/{user_id}", status_code=204)
async def unblock_user(user_id: uuid.UUID, user: UserDep, db: DbDep) -> Response:
    await db.execute(
        delete(Block)
        .where(Block.blocker_id == user.id, Block.blocked_id == user_id)
        .execution_options(synchronize_session=False)
    )
    await db.commit()
    return Response(status_code=204)
