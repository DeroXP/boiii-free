"""The Alembic migrations build exactly the schema the models describe."""

from pathlib import Path

import sqlalchemy as sa
from alembic import command
from alembic.autogenerate import compare_metadata
from alembic.config import Config
from alembic.migration import MigrationContext
from alembic.script import ScriptDirectory

from app.models import Base

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
