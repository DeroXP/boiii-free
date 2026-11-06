"""Crash reports from launchers (contract xerces.crash/1), kept 90 days.

Launchers send one after a game they started crashes, when "Send crash reports" is on. A report
is keyed to a random install id from the X-Xerces-Install header, never to an account, and the
same header deletes every report of that install. No client address is stored or logged; it is
only hashed in memory for the rate limits.
"""

import asyncio
import logging
import math
import uuid

from fastapi import APIRouter, Request, Response
from sqlalchemy import delete, func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.crashes import check_install_id, decode_body, prepare_body
from app.deps import DbDep, ServicesDep
from app.errors import ApiError
from app.log import log_event
from app.models import CrashMinidump, CrashReport, utcnow
from app.ratelimit import Charge
from app.schemas import CrashCreatedOut, CrashesDeletedOut

router = APIRouter(prefix="/v1/crashes", tags=["crashes"])

# POST here gets the larger body limit in app.security.BodySizeLimitMiddleware, which also charges
# ADDRESS_RULE per hashed address before it reads the body.
CRASHES_PATH = "/v1/crashes"
ADDRESS_RULE = "crash_address"
# When the stored reports fill XERCES_CRASH_STORAGE_MB: try again in an hour.
STORAGE_FULL_RETRY = 3600


async def _known(db: AsyncSession, report_id: str) -> uuid.UUID | None:
    return await db.scalar(select(CrashReport.id).where(CrashReport.report_id == report_id))


async def _check_room(db: AsyncSession, ceiling: int, size: int) -> None:
    """Refuse (429) a report that would take the stored total over the ceiling."""
    stored = await db.scalar(select(func.coalesce(func.sum(CrashReport.stored_bytes), 0)))
    if int(stored or 0) + size <= ceiling:
        return
    log_event(
        "crash_storage_full",
        logging.WARNING,
        stored_mib=math.ceil(int(stored or 0) / (1024 * 1024)),
        ceiling_mib=ceiling // (1024 * 1024),
    )
    raise ApiError(
        429,
        "crash_storage_full",
        "The server takes no more crash reports for now. Try again later.",
        headers={"Retry-After": str(STORAGE_FULL_RETRY)},
    )


@router.post("", status_code=201, response_model=CrashCreatedOut)
async def create_crash(
    request: Request, response: Response, db: DbDep, services: ServicesDep
) -> CrashCreatedOut:
    install_id = check_install_id(request.headers.get("x-xerces-install"))
    # The address paid one report already, before the body was read (ADDRESS_RULE). Both are
    # charged before parsing, so rejected and repeated reports cost the sender too.
    services.limiter.enforce("crash_install", install_id)
    raw = await request.body()
    body = await asyncio.to_thread(
        decode_body,
        raw,
        request.headers.get("content-type"),
        request.headers.get("content-encoding"),
        services.tuning.crash_max_body_bytes,
    )
    # What parsing costs grows with the decoded size, so an address also pays by the byte.
    services.limiter.enforce(
        "crash_address_bytes", services.address_key(request.scope), max(1, len(body))
    )
    prepared = await asyncio.to_thread(prepare_body, body)
    del body  # the inflated copy of a gzip body

    known = await _known(db, prepared.report_id)
    if known is None:
        await _check_room(db, services.config.crash_storage_bytes, prepared.stored_bytes)
        # Only reports about to be stored count here, so junk cannot use up the day's room.
        spent: list[Charge] = [
            ("crash_all", "all"),
            ("crash_bytes", "all", float(max(1, prepared.stored_bytes))),
        ]
        services.limiter.enforce_all(spent)
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
            services.limiter.refund_all(spent)
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
                kib=math.ceil(prepared.stored_bytes / 1024),
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
