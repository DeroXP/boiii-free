"""The Alembic migrations build exactly the schema the models describe."""

import asyncio
import contextlib
from collections.abc import AsyncIterator
from pathlib import Path

import httpx
import sqlalchemy as sa
from alembic import command
from alembic.autogenerate import compare_metadata
from alembic.config import Config
from alembic.migration import MigrationContext
from alembic.script import ScriptDirectory
from fastapi import FastAPI

from app.config import Settings
from app.main import create_app
from app.models import Base
from tests.helpers import (
    PASSWORD,
    TestUser,
    client_for,
    make_settings,
    make_tuning,
    next_address,
    register,
    sqlite_url,
)

BACKEND = Path(__file__).resolve().parents[1]


def _config(database: Path) -> Config:
    config = Config(str(BACKEND / "alembic.ini"))
    config.attributes["database_url"] = f"sqlite+aiosqlite:///{database.as_posix()}"
    config.attributes["configure_logging"] = False
    return config


def test_upgrade_head_creates_every_model_table(tmp_path: Path) -> None:
    database = tmp_path / "migrated.db"
    config = _config(database)
    command.upgrade(config, "head")

    engine = sa.create_engine(f"sqlite:///{database.as_posix()}")
    try:
        inspector = sa.inspect(engine)
        tables = set(inspector.get_table_names())
        assert set(Base.metadata.tables) <= tables
        assert tables - set(Base.metadata.tables) == {"alembic_version"}
        for name, table in Base.metadata.tables.items():
            columns = {column["name"] for column in inspector.get_columns(name)}
            assert columns == {column.name for column in table.columns}, name
            indexes = {index["name"] for index in inspector.get_indexes(name)}
            assert indexes == {index.name for index in table.indexes}, name
        with engine.connect() as connection:
            context = MigrationContext.configure(connection, opts={"compare_type": True})
            assert compare_metadata(context, Base.metadata) == []
            head = ScriptDirectory.from_config(config).get_current_head()
            assert context.get_current_revision() == head
    finally:
        engine.dispose()


def test_downgrade_to_base(tmp_path: Path) -> None:
    database = tmp_path / "roundtrip.db"
    config = _config(database)
    command.upgrade(config, "head")
    command.downgrade(config, "base")
    engine = sa.create_engine(f"sqlite:///{database.as_posix()}")
    try:
        assert set(sa.inspect(engine).get_table_names()) == {"alembic_version"}
    finally:
        engine.dispose()
    command.upgrade(config, "head")


@contextlib.asynccontextmanager
async def _running(settings: Settings) -> AsyncIterator[FastAPI]:
    """An app on a migrated database, closed again (engine disposed) before the next migration."""
    app = create_app(settings, tuning=make_tuning())
    lifespan = app.router.lifespan_context(app)
    await lifespan.__aenter__()
    try:
        yield app
    finally:
        await lifespan.__aexit__(None, None, None)


async def _incoming(client: httpx.AsyncClient, user: TestUser) -> list[str]:
    response = await client.get("/v1/friends/requests", headers=user.headers)
    return [r["username"] for r in response.json()["incoming"]]


async def test_rolling_0002_back_and_forward_keeps_hidden_requests_hidden_and_names_held(
    tmp_path: Path,
) -> None:
    database = tmp_path / "rollback.db"
    config = _config(database)
    settings = make_settings(tmp_path, DATABASE_URL=sqlite_url(database))
    # migrations/env.py runs its own event loop, so Alembic gets a thread of its own.
    await asyncio.to_thread(command.upgrade, config, "head")
    async with _running(settings) as app, client_for(app) as client:
        names = ("alice", "shyuser", "friendly", "mallory", "leaver")
        alice, shy, friendly, mallory, leaver = [await register(client, name) for name in names]
        await client.post("/v1/blocks", json={"username": "mallory"}, headers=alice.headers)
        await client.patch("/v1/me/privacy", json={"friend_requests": "nobody"}, headers=shy.headers)
        for name in ("alice", "shyuser", "friendly"):  # two kept hidden, one delivered
            await client.post("/v1/friends/requests", json={"username": name}, headers=mallory.headers)
        gone = await client.request(
            "DELETE", "/v1/me", json={"password": leaver.password}, headers=leaver.headers
        )
        assert gone.status_code == 204

    # Rolling a deploy back across 0002, and forward again.
    await asyncio.to_thread(command.downgrade, config, "0001")
    engine = sa.create_engine(f"sqlite:///{database.as_posix()}")
    try:
        with engine.connect() as connection:
            # 0001 cannot keep a request from its recipient: the hidden ones went, not shown.
            assert connection.scalar(sa.text("SELECT count(*) FROM friend_requests")) == 1
            assert connection.scalar(sa.text("SELECT username_lower FROM deleted_usernames")) == (
                "leaver"
            )
    finally:
        engine.dispose()
    await asyncio.to_thread(command.upgrade, config, "head")

    async with _running(settings) as app, client_for(app) as client:
        assert await _incoming(client, alice) == []
        assert await _incoming(client, shy) == []
        assert await _incoming(client, friendly) == ["mallory"]
        again = await client.post(
            "/v1/auth/register",
            json={"username": "leaver", "password": PASSWORD},
            headers={"X-Forwarded-For": next_address()},
        )
        assert again.status_code == 409  # still held


def test_offline_sql_for_postgres(tmp_path: Path, capsys) -> None:
    config = Config(str(BACKEND / "alembic.ini"))
    config.attributes["database_url"] = "postgresql+asyncpg://user:pass@localhost/xerces"
    config.attributes["configure_logging"] = False
    command.upgrade(config, "head", sql=True)
    sql = capsys.readouterr().out
    assert "CREATE TABLE users" in sql
    assert "id UUID NOT NULL" in sql
    assert "created_at TIMESTAMP WITH TIME ZONE NOT NULL" in sql
    assert "ON DELETE SET NULL" in sql
    assert "ON DELETE CASCADE" in sql
