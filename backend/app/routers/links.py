"""Invite links and knocks (DESIGN.md 10.12).

A host makes a link (XJN- plus 8 characters, kept a day) and shares it anywhere. Someone who
follows it, signed in, "knocks": the host's launcher shows who asks to join, and accepting lets the
host send them a normal game invite for an hour although they are not friends. Nothing here
carries a server address: that still travels only in the invite, to the one person accepted.
"""

import html
import secrets
import uuid
from datetime import timedelta

from fastapi import APIRouter, Path, Request, Response
from fastapi.responses import HTMLResponse
from sqlalchemy import delete, or_, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.deps import DbDep, ServicesDep, UserDep
from app.errors import ApiError, not_found
from app.log import log_event
from app.models import InviteLink, Knock, User, utcnow
from app.presence import blocked_now
from app.schemas import (
    PACK_CODE_ALPHABET,
    KnockOut,
    KnocksOut,
    LinkIn,
    LinkInfoOut,
    LinkOut,
    LinksOut,
    normalize_link_code,
)
from app.social import is_blocked_either_way, usernames

router = APIRouter(tags=["links"])

LINK_TTL = timedelta(days=1)
KNOCK_TTL = timedelta(minutes=10)
# How long after accepting a knock the host may invite that player.
KNOCK_ACCEPTED_TTL = timedelta(hours=1)
CODE_ATTEMPTS = 5

MODE_NAMES = {"zm": "Zombies", "mp": "Multiplayer", "cp": "Campaign"}

# The share page has no script and loads nothing: inline styles only.
JOIN_PAGE_POLICY = (
    "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'none'"
)


def new_link_code() -> str:
    return "XJN-" + "".join(secrets.choice(PACK_CODE_ALPHABET) for _ in range(8))


def link_out(link: InviteLink) -> LinkOut:
    return LinkOut(
        code=link.code,
        path=f"/join/{link.code}",
        mode=link.mode,  # type: ignore[arg-type]
        expires_at=link.expires_at,
    )


def knock_out(knock: Knock, names: dict[uuid.UUID, str], *, outgoing: bool) -> KnockOut:
    status = knock.status
    if outgoing and status == "declined":
        status = "pending"
    return KnockOut(
        id=knock.id,
        from_user_id=knock.from_user_id,
        from_username=names.get(knock.from_user_id, ""),
        to_user_id=knock.to_user_id,
        to_username=names.get(knock.to_user_id, ""),
        mode=knock.mode,  # type: ignore[arg-type]
        status=status,  # type: ignore[arg-type]
        created_at=knock.created_at,
        expires_at=knock.expires_at,
    )


async def _live_link(db: AsyncSession, code: str) -> InviteLink | None:
    normalized = normalize_link_code(code)
    if normalized is None:
        return None
    link = await db.scalar(select(InviteLink).where(InviteLink.code == normalized))
    if link is None or link.expires_at <= utcnow():
        return None
    return link


async def _own_link(db: AsyncSession, user_id: uuid.UUID) -> InviteLink | None:
    return await db.scalar(
        select(InviteLink)
        .where(InviteLink.owner_id == user_id, InviteLink.expires_at > utcnow())
        .order_by(InviteLink.created_at.desc())
        .limit(1)
    )


async def accepted_knock(db: AsyncSession, knocker: uuid.UUID, host: uuid.UUID) -> bool:
    """Whether `knocker` knocked on `host`'s link and the host accepted, within the hour."""
    row = await db.scalar(
        select(Knock.id)
        .where(
            Knock.from_user_id == knocker,
            Knock.to_user_id == host,
            Knock.status == "accepted",
            Knock.expires_at > utcnow(),
        )
        .limit(1)
    )
    return row is not None


# ----------------------------------------------------------------------------- links


@router.post("/v1/links", status_code=201, response_model=LinkOut)
async def create_link(body: LinkIn, user: UserDep, db: DbDep, services: ServicesDep) -> LinkOut:
    """A new link; the caller's old links stop working (one link at a time)."""
    services.limiter.enforce("link_create", str(user.id))
    now = utcnow()
    for _ in range(CODE_ATTEMPTS):
        await db.execute(
            delete(InviteLink)
            .where(InviteLink.owner_id == user.id)
            .execution_options(synchronize_session=False)
        )
        link = InviteLink(
            id=uuid.uuid4(),
            code=new_link_code(),
            owner_id=user.id,
            mode=body.mode,
            created_at=now,
            expires_at=now + LINK_TTL,
        )
        db.add(link)
        try:
            await db.commit()
        except IntegrityError:  # someone took the same code in between; try another
            await db.rollback()
            continue
        log_event("link_created", user_id=str(user.id))
        return link_out(link)
    raise ApiError(503, "unavailable", "Could not allocate a link. Try again.")


@router.get("/v1/links", response_model=LinksOut)
async def my_link(user: UserDep, db: DbDep) -> LinksOut:
    link = await _own_link(db, user.id)
    return LinksOut(link=link_out(link) if link else None)


@router.delete("/v1/links", status_code=204)
async def delete_links(user: UserDep, db: DbDep) -> Response:
    """Stop sharing: every link of the caller stops working. Knocks already made stay."""
    await db.execute(
        delete(InviteLink)
        .where(InviteLink.owner_id == user.id)
        .execution_options(synchronize_session=False)
    )
    await db.commit()
    return Response(status_code=204)


@router.get("/v1/links/{code}", response_model=LinkInfoOut)
async def link_info(
    user: UserDep, db: DbDep, services: ServicesDep, code: str = Path(max_length=32)
) -> LinkInfoOut:
    """Whose link this is, so the launcher can ask "Ask <host> to join?". Signed in only."""
    services.limiter.enforce("link_fetch", str(user.id))
    link = await _live_link(db, code)
    if link is None or await is_blocked_either_way(db, user.id, link.owner_id):
        raise not_found("Link not found.")
    names = await usernames(db, [link.owner_id])
    return LinkInfoOut(
        code=link.code,
        host_user_id=link.owner_id,
        host_username=names.get(link.owner_id, ""),
        mode=link.mode,  # type: ignore[arg-type]
        expires_at=link.expires_at,
    )


# ----------------------------------------------------------------------------- knocks


@router.post("/v1/links/{code}/knock", status_code=201, response_model=KnockOut)
async def knock(
    user: UserDep,
    db: DbDep,
    services: ServicesDep,
    code: str = Path(max_length=32),
) -> KnockOut:
    services.limiter.enforce("knock", str(user.id))
    link = await _live_link(db, code)
    if link is None:
        raise not_found("Link not found.")
    if link.owner_id == user.id:
        raise ApiError(409, "own_link", "This is your own link.")
    host = await db.get(User, link.owner_id)
    if host is None:
        raise not_found("Link not found.")
    services.limiter.enforce("knock_host", f"{user.id}:{host.id}")
    names = {user.id: user.username, host.id: host.username}
    now = utcnow()
    existing = await db.scalar(
        select(Knock)
        .where(Knock.from_user_id == user.id, Knock.to_user_id == host.id, Knock.expires_at > now)
        .order_by(Knock.created_at.desc())
        .limit(1)
    )
    if existing is not None:
        # Knocking again changes nothing and tells the host nothing new: no second notice, and a
        # declined knock still reads "pending" to the one who made it.
        return knock_out(existing, names, outgoing=True)
    row = Knock(
        id=uuid.uuid4(),
        from_user_id=user.id,
        to_user_id=host.id,
        mode=link.mode,
        created_at=now,
        expires_at=now + KNOCK_TTL,
        status="pending",
    )
    if await is_blocked_either_way(db, user.id, host.id):
        # Answered like any other knock, but kept nowhere and shown to nobody: a block is never
        # revealed to the blocked side.
        return knock_out(row, names, outgoing=True)
    db.add(row)
    await db.commit()
    if await blocked_now(db, services.hub, user.id, host.id):
        await db.execute(
            delete(Knock).where(Knock.id == row.id).execution_options(synchronize_session=False)
        )
        await db.commit()
        return knock_out(row, names, outgoing=True)
    out = knock_out(row, names, outgoing=False)
    services.hub.send(host.id, {"type": "knock", **out.model_dump(mode="json")})
    log_event("knock_created", knock_id=str(row.id), user_id=str(user.id))
    return out


@router.get("/v1/knocks", response_model=KnocksOut)
async def list_knocks(user: UserDep, db: DbDep) -> KnocksOut:
    """Pending knocks on the caller's links, and the caller's own knocks until they expire."""
    rows = (
        await db.scalars(
            select(Knock)
            .where(
                or_(Knock.to_user_id == user.id, Knock.from_user_id == user.id),
                Knock.expires_at > utcnow(),
            )
            .order_by(Knock.created_at.desc())
        )
    ).all()
    names = await usernames(db, [r.from_user_id for r in rows] + [r.to_user_id for r in rows])
    return KnocksOut(
        incoming=[
            knock_out(r, names, outgoing=False)
            for r in rows
            if r.to_user_id == user.id and r.status == "pending"
        ],
        outgoing=[knock_out(r, names, outgoing=True) for r in rows if r.from_user_id == user.id],
    )


async def _pending_knock_for(db: AsyncSession, knock_id: uuid.UUID, host: User) -> Knock:
    row = await db.get(Knock, knock_id)
    if (
        row is None
        or row.to_user_id != host.id
        or row.status != "pending"
        or row.expires_at <= utcnow()
    ):
        raise not_found("Knock not found.")
    return row


@router.post("/v1/knocks/{knock_id}/accept", response_model=KnockOut)
async def accept_knock(
    knock_id: uuid.UUID, user: UserDep, db: DbDep, services: ServicesDep
) -> KnockOut:
    """The host may now invite this player (POST /v1/invites) for an hour."""
    row = await _pending_knock_for(db, knock_id, user)
    if await blocked_now(db, services.hub, user.id, row.from_user_id):
        raise not_found("Knock not found.")
    row.status = "accepted"
    row.expires_at = utcnow() + KNOCK_ACCEPTED_TTL
    await db.commit()
    names = await usernames(db, [row.from_user_id, row.to_user_id])
    return knock_out(row, names, outgoing=False)


@router.post("/v1/knocks/{knock_id}/decline", status_code=204)
async def decline_knock(knock_id: uuid.UUID, user: UserDep, db: DbDep) -> Response:
    row = await _pending_knock_for(db, knock_id, user)
    row.status = "declined"
    await db.commit()
    return Response(status_code=204)


# ----------------------------------------------------------------------------- share page


def _join_page(title: str, body: str, status: int) -> HTMLResponse:
    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>{html.escape(title)}</title>
<style>
body {{ margin: 0; min-height: 100vh; display: grid; place-items: center; background: #101114;
  color: #e8e8ea; font: 16px/1.5 system-ui, sans-serif; }}
main {{ max-width: 26rem; padding: 2rem 1.25rem; text-align: center; }}
h1 {{ font-size: 1.5rem; margin: 0 0 .5rem; }}
p {{ color: #b4b4ba; margin: .5rem 0; }}
a.open {{ display: inline-block; margin: 1.25rem 0 .75rem; padding: .75rem 1.5rem;
  border-radius: .5rem; background: #e8a33a; color: #101114; font-weight: 600;
  text-decoration: none; }}
code {{ color: #e8e8ea; }}
</style>
</head>
<body><main>{body}</main></body>
</html>
"""
    return HTMLResponse(
        page, status_code=status, headers={"Content-Security-Policy": JOIN_PAGE_POLICY}
    )


@router.get("/join/{code}", response_class=HTMLResponse, include_in_schema=False)
async def join_page(
    request: Request, db: DbDep, services: ServicesDep, code: str = Path(max_length=32)
) -> HTMLResponse:
    """What a shared link opens in a browser: a button that hands the code to Xerces. It names no
    one, so a link seen by strangers shows them nothing about the host."""
    services.limiter.enforce("join_page", services.address_key(request.scope))
    link = await _live_link(db, code)
    if link is None:
        return _join_page(
            "Invite link expired",
            "<h1>This invite has expired</h1>"
            "<p>Invite links last a day. Ask for a new one.</p>",
            404,
        )
    mode = MODE_NAMES.get(link.mode, "a game")
    safe_code = html.escape(link.code)
    return _join_page(
        "Join a Xerces game",
        f"<h1>You're invited to {html.escape(mode)}</h1>"
        f'<a class="open" href="xerces://join/{safe_code}">Open in Xerces</a>'
        "<p>Xerces asks the host to let you in. You need Xerces installed, signed in, and "
        "&ldquo;Open invite links in Xerces&rdquo; turned on in Friends.</p>"
        f"<p>Or paste <code>{safe_code}</code> into Friends &rsaquo; Join with a link.</p>",
        200,
    )
