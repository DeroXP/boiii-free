"""Background housekeeping, run inside the app every 10 minutes."""

import asyncio
import logging
from datetime import datetime, timedelta
from typing import TYPE_CHECKING

from sqlalchemy import delete
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker

from app.log import log_event
from app.models import (
    DeletedUsername,
    FriendRequest,
    Invite,
    Pack,
    RefreshToken,
    Report,
    utcnow,
)

if TYPE_CHECKING:
    from app.deps import Services

REPORT_RETENTION = timedelta(days=90)


async def run_cleanup(
    sessionmaker: async_sessionmaker[AsyncSession], now: datetime | None = None
) -> dict[str, int]:
    """Delete expired invites, friend requests, refresh tokens, packs and held names of deleted
    accounts, and old reports."""
    now = now or utcnow()
    statements = {
        "invites": delete(Invite).where(Invite.expires_at < now),
        "friend_requests": delete(FriendRequest).where(FriendRequest.expires_at < now),
        "refresh_tokens": delete(RefreshToken).where(RefreshToken.expires_at < now),
        "packs": delete(Pack).where(Pack.expires_at < now),
        "reports": delete(Report).where(Report.created_at < now - REPORT_RETENTION),
        "deleted_usernames": delete(DeletedUsername).where(DeletedUsername.expires_at < now),
    }
    counts: dict[str, int] = {}
    async with sessionmaker() as db:
        for name, statement in statements.items():
            result = await db.execute(statement.execution_options(synchronize_session=False))
            counts[name] = result.rowcount or 0
        await db.commit()
    return counts


async def cleanup_loop(services: "Services", interval: float) -> None:
    """Run until cancelled (on shutdown); one failed pass never stops the loop."""
    while True:
        try:
            counts = await run_cleanup(services.sessionmaker)
            pruned = services.limiter.prune()
            log_event("cleanup", deleted=counts, rate_buckets_pruned=pruned)
        except asyncio.CancelledError:
            raise
        except Exception:
            logging.getLogger("xerces_backend").exception("cleanup_failed")
        await asyncio.sleep(interval)
