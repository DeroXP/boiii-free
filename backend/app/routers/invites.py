"""Game invites between friends. They live 10 minutes."""

import uuid
from datetime import timedelta

from fastapi import APIRouter, Response
from sqlalchemy import or_, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.deps import DbDep, ServicesDep, UserDep
from app.errors import ApiError, not_found
from app.log import log_event
from app.models import Invite, User, utcnow
from app.schemas import InviteIn, InviteOut, InvitesOut
from app.social import are_friends, is_blocked_either_way, usernames

router = APIRouter(prefix="/v1/invites", tags=["invites"])

INVITE_TTL = timedelta(minutes=10)


def invite_out(invite: Invite, names: dict[uuid.UUID, str]) -> InviteOut:
    return InviteOut(
        id=invite.id,
        from_user_id=invite.from_user_id,
        from_username=names.get(invite.from_user_id, ""),
        to_user_id=invite.to_user_id,
        to_username=names.get(invite.to_user_id, ""),
        session=invite.session,
        pack_code=invite.pack_code,
        status=invite.status,  # type: ignore[arg-type]
        created_at=invite.created_at,
        expires_at=invite.expires_at,
    )


@router.post("", status_code=201, response_model=InviteOut)
async def create_invite(
    body: InviteIn, user: UserDep, db: DbDep, services: ServicesDep
) -> InviteOut:
    services.limiter.enforce("invite", str(user.id))
    target = await db.get(User, body.to_user_id)
    allowed = (
        target is not None
        and target.id != user.id
        and target.privacy_invites == "friends"
        and await are_friends(db, user.id, target.id)
        and not await is_blocked_either_way(db, user.id, target.id)
    )
    if not allowed or target is None:
        # One message for every reason, so the answer reveals nothing about privacy or blocks.
        raise ApiError(403, "forbidden", "You cannot invite this user.")
    services.limiter.enforce("invite_recipient", f"{user.id}:{target.id}")
    now = utcnow()
    invite = Invite(
        id=uuid.uuid4(),
        from_user_id=user.id,
        to_user_id=target.id,
        session=body.session,
        pack_code=body.pack_code,
        created_at=now,
        expires_at=now + INVITE_TTL,
        status="pending",
    )
    db.add(invite)
    await db.commit()
    out = invite_out(invite, {user.id: user.username, target.id: target.username})
    services.hub.send(target.id, {"type": "invite", **out.model_dump(mode="json")})
    log_event("invite_created", invite_id=str(invite.id), user_id=str(user.id))
    return out


@router.get("", response_model=InvitesOut)
async def list_invites(user: UserDep, db: DbDep) -> InvitesOut:
    """Pending incoming invites, and outgoing ones (any status) until they expire."""
    now = utcnow()
    rows = (
        await db.scalars(
            select(Invite)
            .where(
                or_(Invite.to_user_id == user.id, Invite.from_user_id == user.id),
                Invite.expires_at > now,
            )
            .order_by(Invite.created_at.desc())
        )
    ).all()
    names = await usernames(db, [r.from_user_id for r in rows] + [r.to_user_id for r in rows])
    return InvitesOut(
        incoming=[
            invite_out(r, names) for r in rows if r.to_user_id == user.id and r.status == "pending"
        ],
        outgoing=[invite_out(r, names) for r in rows if r.from_user_id == user.id],
    )


async def _pending_invite_for(db: AsyncSession, invite_id: uuid.UUID, user: User) -> Invite:
    invite = await db.get(Invite, invite_id)
    if (
        invite is None
        or invite.to_user_id != user.id
        or invite.status != "pending"
        or invite.expires_at <= utcnow()
    ):
        raise not_found("Invite not found.")
    return invite


@router.post("/{invite_id}/accept", response_model=InviteOut)
async def accept_invite(invite_id: uuid.UUID, user: UserDep, db: DbDep) -> InviteOut:
    invite = await _pending_invite_for(db, invite_id, user)
    if not await are_friends(db, user.id, invite.from_user_id):
        raise not_found("Invite not found.")
    invite.status = "accepted"
    await db.commit()
    names = await usernames(db, [invite.from_user_id, invite.to_user_id])
    return invite_out(invite, names)


@router.post("/{invite_id}/decline", status_code=204)
async def decline_invite(invite_id: uuid.UUID, user: UserDep, db: DbDep) -> Response:
    invite = await _pending_invite_for(db, invite_id, user)
    invite.status = "declined"
    await db.commit()
    return Response(status_code=204)
