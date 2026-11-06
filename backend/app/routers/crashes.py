"""Crash reports from launchers (contract xerces.crash/1), kept 90 days.

Launchers send one after a game they started crashes, when "Send crash reports" is on. A report
is keyed to a random install id from the X-Xerces-Install header, never to an account, and the
same header deletes every report of that install. No client address is stored or logged; it is
only hashed in memory for the rate limits.
"""

import asyncio
import uuid

from fastapi import APIRouter, Request, Response
from sqlalchemy import delete, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.crashes import check_install_id, prepare_report
from app.deps import DbDep, ServicesDep
from app.log import log_event
from app.models import CrashMinidump, CrashReport, utcnow
from app.schemas import CrashCreatedOut, CrashesDeletedOut

router = APIRouter(prefix="/v1/crashes", tags=["crashes"])

# The path of the larger body limit in app.security.BodySizeLimitMiddleware.
CRASHES_PATH = "/v1/crashes"


async def _known(db: AsyncSession, report_id: str) -> uuid.UUID | None:
    return await db.scalar(select(CrashReport.id).where(CrashReport.report_id == report_id))


@router.post("", status_code=201, response_model=CrashCreatedOut)
async def create_crash(
    request: Request, response: Response, db: DbDep, services: ServicesDep
) -> CrashCreatedOut:
    install_id = check_install_id(request.headers.get("x-xerces-install"))
    # Charged before parsing, so rejected and repeated reports cost the sender too.
    services.limiter.enforce_all(
        [
            ("crash_address", services.address_key(request.scope)),
            ("crash_install", install_id),
        ]
    )
    raw = await request.body()
    prepared = await asyncio.to_thread(
        prepare_report,
        raw,
        request.headers.get("content-type"),
        request.headers.get("content-encoding"),
        services.tuning.crash_max_body_bytes,
    )

    known = await _known(db, prepared.report_id)
    if known is None:
        # Only reports about to be stored count here, so junk cannot use up the day's room.
        services.limiter.enforce("crash_all", "all")
        row = CrashReport(
            id=uuid.uuid4(), install_id=install_id, received_at=utcnow(), **prepared.columns
        )
        db.add(row)
        try:
            await db.flush()
            if prepared.minidump is not None:
                db.add(
                    CrashMinidump(
                        crash_id=row.id, size=len(prepared.minidump), data=prepared.minidump
                    )
                )
            await db.commit()
        except IntegrityError:
            # The same report arrived twice at once and the other copy was stored first.
            await db.rollback()
            services.limiter.refund("crash_all", "all")
            known = await _known(db, prepared.report_id)
            if known is None:
                raise
        else:
            log_event(
                "crash_received",
                crash_id=str(row.id),
                kind=row.kind,
                signature=row.signature,
                version=row.version,
                minidump=prepared.minidump is not None,
            )
            return CrashCreatedOut(id=row.id, duplicate=False)

    response.status_code = 200
    log_event("crash_duplicate", crash_id=str(known))
    return CrashCreatedOut(id=known, duplicate=True)


@router.delete("", response_model=CrashesDeletedOut)
async def delete_crashes(request: Request, db: DbDep, services: ServicesDep) -> CrashesDeletedOut:
    install_id = check_install_id(request.headers.get("x-xerces-install"))
    services.limiter.enforce("crash_delete", services.address_key(request.scope))
    # Minidumps go with their reports (ON DELETE CASCADE).
    result = await db.execute(
        delete(CrashReport)
        .where(CrashReport.install_id == install_id)
        .execution_options(synchronize_session=False)
    )
    await db.commit()
    deleted = result.rowcount or 0
    log_event("crashes_deleted", count=deleted)  # without the install id
    return CrashesDeletedOut(deleted=deleted)
