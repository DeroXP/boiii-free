"""Reports about other users, kept 90 days for moderators (there is no moderator UI yet)."""

import uuid

from fastapi import APIRouter

from app.deps import DbDep, ServicesDep, UserDep
from app.log import log_event
from app.models import Report, utcnow
from app.schemas import ReportIn, StatusOut
from app.social import find_user

router = APIRouter(prefix="/v1/reports", tags=["reports"])


@router.post("", status_code=202, response_model=StatusOut)
async def create_report(
    body: ReportIn, user: UserDep, db: DbDep, services: ServicesDep
) -> StatusOut:
    services.limiter.enforce("report", str(user.id))
    target = await find_user(db, body.username)
    # Same answer whether or not the user exists.
    if target is not None and target.id != user.id:
        report = Report(
            id=uuid.uuid4(),
            reporter_id=user.id,
            reported_id=target.id,
            reason=body.reason,
            details=body.details,
            created_at=utcnow(),
            status="open",
        )
        db.add(report)
        await db.commit()
        log_event("report_created", report_id=str(report.id), reason=body.reason)
    return StatusOut(status="received")
