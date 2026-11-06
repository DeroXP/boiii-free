"""Admin routes for reading crash reports (the project owner only; scripts/crashes.py uses them).

They need `Authorization: Bearer <XERCES_ADMIN_TOKEN>`. When that variable is unset they answer
404 like any unknown path. Tokens are compared as SHA-256 digests in constant time, and wrong ones
are rate limited per hashed address.
"""

import base64
import binascii
import hmac
import logging
import uuid
from collections import Counter
from datetime import UTC, datetime
from typing import Annotated, Any

from fastapi import APIRouter, Depends, Path, Query, Request, Response
from sqlalchemy import and_, or_, select

from app.config import hash_admin_token
from app.deps import DbDep, ServicesDep
from app.errors import ApiError, not_found
from app.log import log_event
from app.models import CrashMinidump, CrashReport, iso
from app.schemas import (
    CRASH_SCHEMA,
    CountOut,
    CrashGroupOut,
    CrashGroupsOut,
    CrashListOut,
    CrashModCountOut,
    CrashSummaryOut,
)

TOP = 5


async def require_admin(request: Request, services: ServicesDep) -> None:
    expected = services.config.admin_token_hash
    if expected is None:
        raise ApiError(404, "not_found", "Not Found")  # the same answer as an unknown path
    key = services.address_key(request.scope)
    # Paid first and given back when the token is right, as for logins.
    services.limiter.enforce("admin_auth", key)
    scheme, _, token = request.headers.get("authorization", "").partition(" ")
    if scheme.lower() == "bearer" and hmac.compare_digest(
        hash_admin_token(token.strip()), expected
    ):
        services.limiter.refund("admin_auth", key)
        return
    log_event("admin_auth_failed", logging.WARNING)
    raise ApiError(
        401,
        "unauthorized",
        "The admin token is missing or wrong.",
        headers={"WWW-Authenticate": "Bearer"},
    )


# A router dependency runs before the routes' own query checks, so with no token configured even
# a malformed request gets the plain 404.
router = APIRouter(
    prefix="/v1/admin",
    tags=["admin"],
    dependencies=[Depends(require_admin)],
    include_in_schema=False,
)

Since = Annotated[
    datetime | None, Query(description="Only reports received at or after this time (UTC).")
]


def _as_utc(value: datetime) -> datetime:
    """Times without an offset are taken as UTC."""
    try:
        return value.replace(tzinfo=UTC) if value.tzinfo is None else value.astimezone(UTC)
    except OverflowError:
        raise ApiError(400, "invalid_time", "The time is out of range.") from None


def _encode_cursor(received_at: datetime, crash_id: uuid.UUID) -> str:
    text = f"{iso(received_at)}|{crash_id}"
    return base64.urlsafe_b64encode(text.encode("ascii")).rstrip(b"=").decode("ascii")


def _decode_cursor(cursor: str) -> tuple[datetime, uuid.UUID]:
    try:
        padded = cursor + "=" * (-len(cursor) % 4)
        text = base64.urlsafe_b64decode(padded.encode("ascii")).decode("ascii")
        moment, _, crash_id = text.partition("|")
        return _as_utc(datetime.fromisoformat(moment)), uuid.UUID(crash_id)
    except (binascii.Error, UnicodeError, ValueError):
        raise ApiError(400, "invalid_cursor", "cursor is not valid.") from None


_SUMMARY_COLUMNS = (
    CrashReport.id,
    CrashReport.report_id,
    CrashReport.install_id,
    CrashReport.received_at,
    CrashReport.created_utc,
    CrashReport.version,
    CrashReport.commit,
    CrashReport.game_build,
    CrashReport.kind,
    CrashReport.code,
    CrashReport.module,
    CrashReport.offset,
    CrashReport.signature,
    CrashReport.phase,
    CrashReport.mode,
    CrashReport.map,
    CrashMinidump.size,
)


@router.get("/crashes", response_model=CrashListOut)
async def list_crashes(
    db: DbDep,
    since: Since = None,
    signature: Annotated[str | None, Query(pattern=r"^[0-9a-f]{16}$")] = None,
    version: Annotated[str | None, Query(max_length=64)] = None,
    limit: Annotated[int, Query(ge=1, le=200)] = 50,
    cursor: Annotated[str | None, Query(max_length=200)] = None,
) -> CrashListOut:
    """Newest first, metadata only. Pass `next_cursor` back as `cursor` for the next page."""
    statement = select(*_SUMMARY_COLUMNS).outerjoin(
        CrashMinidump, CrashMinidump.crash_id == CrashReport.id
    )
    if since is not None:
        statement = statement.where(CrashReport.received_at >= _as_utc(since))
    if signature is not None:
        statement = statement.where(CrashReport.signature == signature)
    if version is not None:
        statement = statement.where(CrashReport.version == version)
    if cursor is not None:
        received_at, crash_id = _decode_cursor(cursor)
        statement = statement.where(
            or_(
                CrashReport.received_at < received_at,
                and_(CrashReport.received_at == received_at, CrashReport.id < crash_id),
            )
        )
    statement = statement.order_by(CrashReport.received_at.desc(), CrashReport.id.desc())
    rows = (await db.execute(statement.limit(limit + 1))).all()
    items = [
        CrashSummaryOut(
            id=row.id,
            report_id=row.report_id,
            install_id=row.install_id,
            received_at=row.received_at,
            created_utc=row.created_utc,
            version=row.version,
            commit=row.commit,
            game_build=row.game_build,
            kind=row.kind,
            code=row.code,
            module=row.module,
            offset=row.offset,
            signature=row.signature,
            phase=row.phase,
            mode=row.mode,
            map=row.map,
            has_minidump=row.size is not None,
            minidump_size=row.size,
        )
        for row in rows[:limit]
    ]
    next_cursor = None
    if len(rows) > limit:
        last = items[-1]
        next_cursor = _encode_cursor(last.received_at, last.id)
    return CrashListOut(items=items, next_cursor=next_cursor)


@router.get("/crashes/{crash_id}")
async def get_crash(db: DbDep, crash_id: Annotated[uuid.UUID, Path()]) -> dict[str, Any]:
    """The whole report as stored (after the server's scrub), without the minidump's bytes."""
    row = await db.get(CrashReport, crash_id)
    if row is None:
        raise not_found("Crash report not found.")
    size = await db.scalar(select(CrashMinidump.size).where(CrashMinidump.crash_id == row.id))
    return {
        "id": str(row.id),
        "received_at": iso(row.received_at),
        "install_id": row.install_id,
        "signature": row.signature,
        "has_minidump": size is not None,
        "minidump_size": size,
        "schema": CRASH_SCHEMA,
        "report_id": row.report_id,
        "created_utc": iso(row.created_utc),
        "xerces": {"version": row.version, "commit": row.commit, "dirty": row.dirty},
        "game": {"build": row.game_build, "steam_build": row.steam_build},
        "system": row.system,
        "launch": {
            "mode": row.mode,
            "map": row.map,
            "plan_id": row.plan_id,
            "mods": row.mods,
            "flags": row.flags,
            "safe_mode": row.safe_mode,
        },
        "crash": row.crash,
        "logs": row.logs,
    }


@router.get("/crashes/{crash_id}/minidump")
async def get_minidump(db: DbDep, crash_id: Annotated[uuid.UUID, Path()]) -> Response:
    data = await db.scalar(select(CrashMinidump.data).where(CrashMinidump.crash_id == crash_id))
    if data is None:
        raise not_found("This report has no minidump.")
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={"Content-Disposition": f'attachment; filename="{crash_id}.dmp"'},
    )


def _ranked(counts: Counter[str], top: int | None = None) -> list[tuple[str, int]]:
    """Most frequent first; ties in name order, so the answer never depends on row order."""
    return sorted(counts.items(), key=lambda item: (-item[1], item[0]))[:top]


class _Group:
    __slots__ = (
        "count",
        "crash",
        "first_seen",
        "installs",
        "kind",
        "last_seen",
        "maps",
        "mod_names",
        "mods",
        "module",
        "offset",
        "sample_id",
        "signature",
        "versions",
    )

    def __init__(self, row: Any) -> None:
        # Rows come newest first, so the first one is the sample and the latest sighting.
        self.signature: str = row.signature
        self.sample_id: uuid.UUID = row.id
        self.kind: str = row.kind
        self.module: str | None = row.module
        self.offset: str | None = row.offset
        self.crash: dict[str, Any] = row.crash or {}
        self.last_seen: datetime = row.received_at
        self.first_seen: datetime = row.received_at
        self.count = 0
        self.installs: set[str] = set()
        self.versions: Counter[str] = Counter()
        self.maps: Counter[str] = Counter()
        self.mods: Counter[str] = Counter()
        self.mod_names: dict[str, tuple[str | None, str | None]] = {}

    def add(self, row: Any) -> None:
        self.count += 1
        self.first_seen = min(self.first_seen, row.received_at)
        self.last_seen = max(self.last_seen, row.received_at)
        self.installs.add(row.install_id)
        self.versions[row.version] += 1
        if row.map:
            self.maps[row.map] += 1
        seen: set[str] = set()
        for mod in row.mods or ():
            if not isinstance(mod, dict):
                continue
            mod_id, name = mod.get("id"), mod.get("name")
            key = mod_id or name
            if not isinstance(key, str) or key in seen:
                continue
            seen.add(key)
            self.mods[key] += 1
            self.mod_names.setdefault(key, (mod_id, name))

    def out(self) -> CrashGroupOut:
        frames = self.crash.get("frames") or []
        return CrashGroupOut(
            signature=self.signature,
            count=self.count,
            installs=len(self.installs),
            first_seen=self.first_seen,
            last_seen=self.last_seen,
            kind=self.kind,
            module=self.module,
            offset=self.offset,
            frames=[frame for frame in frames[:3] if isinstance(frame, str)],
            versions=[CountOut(value=v, count=n) for v, n in _ranked(self.versions)],
            top_mods=[
                CrashModCountOut(id=self.mod_names[k][0], name=self.mod_names[k][1], count=n)
                for k, n in _ranked(self.mods, TOP)
            ],
            top_maps=[CountOut(value=m, count=n) for m, n in _ranked(self.maps, TOP)],
            sample_id=self.sample_id,
        )


@router.get("/crash-groups", response_model=CrashGroupsOut)
async def crash_groups(
    db: DbDep,
    since: Since = None,
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
) -> CrashGroupsOut:
    """Reports grouped by signature, the most frequent first."""
    statement = select(
        CrashReport.id,
        CrashReport.signature,
        CrashReport.install_id,
        CrashReport.received_at,
        CrashReport.version,
        CrashReport.kind,
        CrashReport.module,
        CrashReport.offset,
        CrashReport.map,
        CrashReport.mods,
        CrashReport.crash,
    ).order_by(CrashReport.received_at.desc(), CrashReport.id.desc())
    if since is not None:
        statement = statement.where(CrashReport.received_at >= _as_utc(since))
    groups: dict[str, _Group] = {}
    total = 0
    # Streamed, so 90 days of reports never sit in memory at once; only the tallies do.
    result = await db.stream(statement.execution_options(yield_per=500))
    async for row in result:
        total += 1
        group = groups.get(row.signature)
        if group is None:
            group = groups[row.signature] = _Group(row)
        group.add(row)
    ranked = sorted(groups.values(), key=lambda g: (-g.count, -g.last_seen.timestamp()))
    return CrashGroupsOut(reports=total, groups=[group.out() for group in ranked[:limit]])
