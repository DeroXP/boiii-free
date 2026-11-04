"""Alembic environment (async engine).

The URL comes from DATABASE_URL (postgres:// and postgresql:// are converted for asyncpg). Tests
pass `database_url` and `configure_logging` through `Config.attributes` instead.
"""

import asyncio

from alembic import context
from sqlalchemy import pool
from sqlalchemy.engine import Connection
from sqlalchemy.ext.asyncio import create_async_engine

from app.config import ConfigError, Settings, resolve_database_url
from app.log import configure_logging
from app.models import Base

config = context.config
target_metadata = Base.metadata

if config.attributes.get("configure_logging", True):
    configure_logging()


def _database_url() -> str:
    url = config.attributes.get("database_url")
    return url if url else resolve_database_url(Settings())


def _configure(**kwargs: object) -> None:
    context.configure(target_metadata=target_metadata, compare_type=True, **kwargs)


def run_migrations_offline() -> None:
    """Emit SQL to stdout instead of running it (`alembic upgrade head --sql`)."""
    _configure(url=_database_url(), literal_binds=True, dialect_opts={"paramstyle": "named"})
    with context.begin_transaction():
        context.run_migrations()


def _run_sync(connection: Connection) -> None:
    # SQLite cannot ALTER most things in place; batch mode copies the table instead.
    _configure(connection=connection, render_as_batch=connection.dialect.name == "sqlite")
    with context.begin_transaction():
        context.run_migrations()


async def _run_async() -> None:
    url = _database_url()
    try:
        engine = create_async_engine(url, poolclass=pool.NullPool, hide_parameters=True)
    except Exception as exc:
        # Errors from parsing the URL can quote it, password included: name the type only.
        raise ConfigError(f"DATABASE_URL could not be used ({type(exc).__name__}).") from None
    try:
        async with engine.connect() as connection:
            await connection.run_sync(_run_sync)
    finally:
        await engine.dispose()


def run_migrations_online() -> None:
    asyncio.run(_run_async())


if context.is_offline_mode():
    run_migrations_offline()
else:
    run_migrations_online()
