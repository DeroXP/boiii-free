"""Async engine and sessions."""

from typing import Any

from sqlalchemy import event
from sqlalchemy.ext.asyncio import (
    AsyncEngine,
    AsyncSession,
    async_sessionmaker,
    create_async_engine,
)


def create_engine(database_url: str) -> AsyncEngine:
    # hide_parameters keeps bound values (password hashes, emails) out of error messages and logs.
    if database_url.startswith("sqlite"):
        engine = create_async_engine(database_url, hide_parameters=True)
        event.listen(engine.sync_engine, "connect", _sqlite_on_connect)
        return engine
    return create_async_engine(
        database_url,
        hide_parameters=True,
        pool_size=5,
        max_overflow=5,
        pool_pre_ping=True,
        pool_recycle=1800,
        connect_args={
            "timeout": 10,
            "command_timeout": 30,
            "server_settings": {"application_name": "xerces-backend"},
        },
    )


def _sqlite_on_connect(dbapi_connection: Any, _record: Any) -> None:
    # SQLite ignores foreign keys (and so ON DELETE CASCADE) unless asked on every connection.
    cursor = dbapi_connection.cursor()
    cursor.execute("PRAGMA foreign_keys=ON")
    cursor.close()


def create_sessionmaker(engine: AsyncEngine) -> async_sessionmaker[AsyncSession]:
    return async_sessionmaker(engine, expire_on_commit=False)
