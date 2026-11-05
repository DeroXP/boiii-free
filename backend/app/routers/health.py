"""Liveness (with a database round trip) and service metadata."""

import asyncio
import logging

from fastapi import APIRouter
from fastapi.responses import JSONResponse
from sqlalchemy import text

from app import __version__
from app.deps import ServicesDep
from app.errors import error_response
from app.log import log_event
from app.models import iso, utcnow

router = APIRouter(tags=["health"])


@router.get("/healthz")
async def healthz(services: ServicesDep) -> JSONResponse:
    try:
        async with asyncio.timeout(5):
            async with services.engine.connect() as connection:
                await connection.execute(text("SELECT 1"))
    except Exception as exc:
        log_event("health_database_unavailable", logging.WARNING, error=type(exc).__name__)
        return error_response(503, "database_unavailable", "Database is unavailable.")
    return JSONResponse({"ok": True})


@router.get("/v1/meta")
async def meta() -> dict[str, object]:
    return {"service": "xerces-backend", "version": __version__, "api": 1, "time": iso(utcnow())}
