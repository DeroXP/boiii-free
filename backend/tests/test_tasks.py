"""Background cleanup."""

import asyncio
import uuid
from datetime import timedelta
from pathlib import Path

from fastapi import FastAPI
from sqlalchemy import func, select

from app.main import create_app
from app.models import (
    CrashMinidump,
    CrashReport,
    DeletedUsername,
    FriendRequest,
    Invite,
    Pack,
    RefreshToken,
    Report,
    User,
    utcnow,
)
from app.tasks import run_cleanup
from tests.helpers import create_schema, make_settings, make_tuning, services_of


async def _user(db, name: str) -> User:
    user = User(
        id=uuid.uuid4(),
        username=name,
        username_lower=name,
        password_hash="x",
        privacy_friend_requests="everyone",
        privacy_presence="friends",
        privacy_invites="friends",
        created_at=utcnow(),
    )
    db.add(user)
    await db.flush()
    return user


async def test_cleanup_deletes_only_expired_rows(app: FastAPI) -> None:
    now = utcnow()
    past, future = now - timedelta(seconds=1), now + timedelta(days=1)
    sessionmaker = services_of(app).sessionmaker
    async with sessionmaker() as db:
        a = await _user(db, "alice")
        b = await _user(db, "bob")
        for when, tag in ((past, "old"), (future, "new")):
            db.add(Invite(from_user_id=a.id, to_user_id=b.id, session=tag, created_at=now, expires_at=when, status="pending"))
            db.add(RefreshToken(user_id=a.id, token_hash=tag * 16, family_id=uuid.uuid4(), created_at=now, expires_at=when))
            db.add(Pack(code=f"XRC-{'0' if tag == 'old' else '1'}0000000", owner_id=a.id, data={}, created_at=now, expires_at=when))
        db.add(FriendRequest(from_user_id=a.id, to_user_id=b.id, created_at=now, expires_at=past))
        db.add(FriendRequest(from_user_id=b.id, to_user_id=a.id, created_at=now, expires_at=future))
        db.add(Report(reporter_id=a.id, reported_id=b.id, reason="spam", created_at=now - timedelta(days=91), status="open"))
        db.add(Report(reporter_id=a.id, reported_id=b.id, reason="other", created_at=now - timedelta(days=89), status="open"))
        db.add(DeletedUsername(username_lower="gone", expires_at=past))
        db.add(DeletedUsername(username_lower="held", expires_at=future))
        await db.commit()

    counts = await run_cleanup(sessionmaker)
    assert counts == {
        "invites": 1,
        "friend_requests": 1,
        "refresh_tokens": 1,
        "packs": 1,
        "reports": 1,
        "deleted_usernames": 1,
        "crash_reports": 0,
    }

    async with sessionmaker() as db:
        assert await db.scalar(select(Invite.session)) == "new"
        assert await db.scalar(select(func.count()).select_from(RefreshToken)) == 1
        assert await db.scalar(select(Pack.code)) == "XRC-10000000"
        assert await db.scalar(select(FriendRequest.from_user_id)) == b.id
        assert await db.scalar(select(Report.reason)) == "other"
        assert await db.scalar(select(DeletedUsername.username_lower)) == "held"


async def test_cleanup_loop_keeps_running_and_stops_at_shutdown(tmp_path: Path) -> None:
    app = create_app(make_settings(tmp_path), tuning=make_tuning(cleanup_interval=0.05))
    await create_schema(app)
    sessionmaker = services_of(app).sessionmaker
    lifespan = app.router.lifespan_context(app)
    await lifespan.__aenter__()
    try:
        task = app.state.cleanup_task
        assert task is not None and not task.done()
        # This pack expires after startup, so only a later pass of the loop can delete it.
        async with sessionmaker() as db:
            now = utcnow()
            db.add(Pack(code="XRC-00000000", data={}, created_at=now, expires_at=now))
            await db.commit()
        for _ in range(150):
            async with sessionmaker() as db:
                left = await db.scalar(select(func.count()).select_from(Pack))
            if left == 0:
                break
            await asyncio.sleep(0.02)
        assert left == 0
    finally:
        await lifespan.__aexit__(None, None, None)
    assert task.cancelled()


def _crash(received_at, report_id: str) -> CrashReport:
    return CrashReport(
        id=uuid.uuid4(),
        report_id=report_id,
        install_id="0" * 32,
        created_utc=received_at,
        received_at=received_at,
        version="0.1.0-beta.1",
        kind="exception",
        signature="0" * 16,
        crash={"kind": "exception"},
    )


async def test_crash_reports_are_kept_90_days(app: FastAPI) -> None:
    now = utcnow()
    sessionmaker = services_of(app).sessionmaker
    old = _crash(now - timedelta(days=90, minutes=1), "a" * 32)
    young = _crash(now - timedelta(days=89, hours=23), "b" * 32)
    # The client's own clock does not count: a report "made" long ago but just received stays.
    fresh = _crash(now, "c" * 32)
    fresh.created_utc = now - timedelta(days=400)
    async with sessionmaker() as db:
        db.add_all([old, young, fresh])
        await db.flush()
        db.add(CrashMinidump(crash_id=old.id, size=4, data=b"MDMP"))
        db.add(CrashMinidump(crash_id=young.id, size=4, data=b"MDMP"))
        await db.commit()

    counts = await run_cleanup(sessionmaker)
    assert counts["crash_reports"] == 1
    async with sessionmaker() as db:
        left = set((await db.scalars(select(CrashReport.report_id))).all())
        dumps = set((await db.scalars(select(CrashMinidump.crash_id))).all())
    assert left == {"b" * 32, "c" * 32}
    assert dumps == {young.id}  # the old report's minidump went with it
