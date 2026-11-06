"""Application factory.

`uvicorn app.main:app` builds the app from the environment on first access (PEP 562), so importing
this module has no side effects and tests can call `create_app()` with their own settings.
"""

import asyncio
import contextlib
import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI

from app import __version__
from app.config import ConfigError, Settings, Tuning, resolve
from app.db import create_engine, create_sessionmaker
from app.deps import Services
from app.errors import install_error_handlers
from app.log import configure_logging, log_event
from app.passwords import PasswordService
from app.presence import PresenceHub
from app.ratelimit import RateLimiter
from app.routers import (
    admin,
    auth,
    blocks,
    crashes,
    friends,
    health,
    invites,
    me,
    packs,
    reports,
    ws,
)
from app.security import BodySizeLimitMiddleware, RequestLogMiddleware, SecurityHeadersMiddleware
from app.tasks import cleanup_loop
from app.tokens import AccessTokens

# FastAPI's built-in OpenTelemetry would record client addresses; this service records none.
_NO_TELEMETRY: Any = {
    "tracing": False,
    "metrics": False,
    "logs": False,
    "operation_spans": False,
    "auto_configure": False,
}


def create_app(settings: Settings | None = None, *, tuning: Tuning | None = None) -> FastAPI:
    tuning = tuning or Tuning()
    if tuning.configure_logging:
        configure_logging()
    try:
        config = resolve(settings if settings is not None else Settings())
    except ConfigError as exc:
        # The message names variables only, never their values.
        log_event("config_error", logging.ERROR, detail=str(exc))
        raise
    for warning in config.warnings:
        log_event("config_warning", logging.WARNING, detail=warning)

    try:
        engine = create_engine(config.database_url)
    except Exception as exc:
        # Errors from parsing the URL can quote it, password included: name the type only.
        error = ConfigError(f"DATABASE_URL could not be used ({type(exc).__name__}).")
        log_event("config_error", logging.ERROR, detail=str(error))
        raise error from None
    services = Services(
        config=config,
        tuning=tuning,
        engine=engine,
        sessionmaker=create_sessionmaker(engine),
        passwords=PasswordService(
            time_cost=config.argon2_time_cost,
            memory_kib=config.argon2_memory_kib,
            parallelism=config.argon2_parallelism,
            max_concurrency=tuning.hash_concurrency,
            max_waiting=tuning.hash_queue,
        ),
        tokens=AccessTokens(config.signing_key, config.key_id, config.verify_keys),
        limiter=RateLimiter(tuning.rate_rules),
        hub=PresenceHub(),
    )

    @asynccontextmanager
    async def lifespan(app_: FastAPI) -> AsyncIterator[None]:
        await services.passwords.warm_up()
        cleanup = None
        if tuning.cleanup_interval:
            cleanup = asyncio.create_task(cleanup_loop(services, tuning.cleanup_interval))
        app_.state.cleanup_task = cleanup  # tests check it ran and was cancelled at shutdown
        log_event("startup", version=__version__, env=config.env, key_id=config.key_id)
        try:
            yield
        finally:
            if cleanup is not None:
                cleanup.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await cleanup
            await services.hub.close_all()
            await engine.dispose()
            log_event("shutdown")

    show_docs = config.env != "production"
    app = FastAPI(
        title="Xerces backend",
        version=__version__,
        lifespan=lifespan,
        docs_url="/docs" if show_docs else None,
        redoc_url=None,
        openapi_url="/openapi.json" if show_docs else None,
        telemetry=_NO_TELEMETRY,
    )
    app.state.services = services
    install_error_handlers(app)
    # Last added runs first: log -> headers -> body limit -> routes.
    app.add_middleware(
        BodySizeLimitMiddleware,
        max_bytes=tuning.max_body_bytes,
        large_paths={crashes.CRASHES_PATH: tuning.crash_max_body_bytes},
        large_slots=tuning.crash_upload_slots,
        large_timeout=tuning.crash_body_timeout,
    )
    app.add_middleware(SecurityHeadersMiddleware)
    app.add_middleware(RequestLogMiddleware)
    for module in (health, auth, me, friends, blocks, reports, invites, packs, ws, crashes, admin):
        app.include_router(module.router)
    return app


def __getattr__(name: str) -> Any:
    if name == "app":
        application = create_app()
        globals()["app"] = application
        return application
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
