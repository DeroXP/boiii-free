"""Fixtures: every test gets its own app on a fresh SQLite file, with the lifespan running."""

from collections.abc import AsyncIterator, Mapping
from pathlib import Path
from typing import Any

import httpx
import pytest
from fastapi import FastAPI

from app.config import Settings
from app.main import create_app
from app.ratelimit import Rule
from tests.helpers import AppFactory, client_for, create_schema, make_settings, make_tuning


@pytest.fixture
async def app_factory(tmp_path: Path) -> AsyncIterator[AppFactory]:
    running: list[Any] = []

    async def build(
        *,
        settings: Settings | None = None,
        rules: Mapping[str, Rule] | None = None,
        schema: bool = True,
        **tuning: Any,
    ) -> FastAPI:
        app = create_app(settings or make_settings(tmp_path), tuning=make_tuning(rules, **tuning))
        if schema:
            await create_schema(app)
        lifespan = app.router.lifespan_context(app)
        await lifespan.__aenter__()
        running.append(lifespan)
        return app

    yield build
    for lifespan in reversed(running):
        await lifespan.__aexit__(None, None, None)


@pytest.fixture
async def app(app_factory: AppFactory) -> FastAPI:
    return await app_factory()


@pytest.fixture
async def client(app: FastAPI) -> AsyncIterator[httpx.AsyncClient]:
    async with client_for(app) as c:
        yield c
