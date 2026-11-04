"""Health, metadata and the response hygiene every endpoint shares."""

import re

import httpx
from fastapi import FastAPI

from tests.helpers import PASSWORD, AppFactory, client_for, make_settings, register


async def test_healthz_ok(client: httpx.AsyncClient) -> None:
    response = await client.get("/healthz")
    assert response.status_code == 200
    assert response.json() == {"ok": True}


async def test_healthz_503_when_database_is_down(app_factory: AppFactory, tmp_path) -> None:
    missing = tmp_path / "no-such-dir" / "db.sqlite"
    settings = make_settings(tmp_path, DATABASE_URL=f"sqlite+aiosqlite:///{missing.as_posix()}")
    app = await app_factory(settings=settings, schema=False)
    async with client_for(app) as client:
        response = await client.get("/healthz")
    assert response.status_code == 503
    assert response.json()["error"]["code"] == "database_unavailable"


async def test_meta(client: httpx.AsyncClient) -> None:
    body = (await client.get("/v1/meta")).json()
    assert body["service"] == "xerces-backend"
    assert body["api"] == 1
    assert re.fullmatch(r"\d+\.\d+\.\d+", body["version"])
    assert body["time"].endswith("Z")


async def test_security_headers_on_auth_and_me(client: httpx.AsyncClient) -> None:
    user = await register(client, "headers")
    for response in (
        await client.post("/v1/auth/login", json={"username": "headers", "password": user.password}),
        await client.get("/v1/me", headers=user.headers),
        await client.get("/v1/meta"),
    ):
        assert response.headers["cache-control"] == "no-store"
        assert response.headers["x-content-type-options"] == "nosniff"
        assert "access-control-allow-origin" not in response.headers


async def test_server_errors_keep_the_security_headers(app_factory: AppFactory) -> None:
    # No tables, so the first query fails. Starlette sends that 500 from outside the middlewares.
    app = await app_factory(schema=False)
    transport = httpx.ASGITransport(app=app, raise_app_exceptions=False)
    async with httpx.AsyncClient(transport=transport, base_url="http://testserver") as client:
        response = await client.post(
            "/v1/auth/register", json={"username": "alice", "password": PASSWORD}
        )
    assert response.status_code == 500
    assert response.json() == {
        "error": {"code": "internal_error", "message": "Something went wrong on our side."}
    }
    assert response.headers["x-content-type-options"] == "nosniff"
    assert response.headers["cache-control"] == "no-store"
    assert response.headers["referrer-policy"] == "no-referrer"


async def test_uniform_error_shape(client: httpx.AsyncClient) -> None:
    missing = await client.get("/v1/nope")
    assert missing.status_code == 404
    assert missing.json() == {"error": {"code": "not_found", "message": "Not Found"}}
    wrong_method = await client.put("/v1/meta")
    assert wrong_method.status_code == 405
    assert wrong_method.json()["error"]["code"] == "method_not_allowed"
    unauthenticated = await client.get("/v1/me")
    assert unauthenticated.status_code == 401
    assert unauthenticated.json()["error"]["code"] == "unauthorized"
    assert unauthenticated.headers["www-authenticate"] == "Bearer"


async def test_body_size_cap(client: httpx.AsyncClient) -> None:
    big = b'{"username": "' + b"a" * (130 * 1024) + b'"}'
    response = await client.post(
        "/v1/auth/login", content=big, headers={"Content-Type": "application/json"}
    )
    assert response.status_code == 413
    assert response.json()["error"]["code"] == "payload_too_large"


async def test_body_size_cap_without_content_length(client: httpx.AsyncClient) -> None:
    async def chunks():
        for _ in range(200):
            yield b"x" * 1024

    response = await client.post(
        "/v1/auth/login", content=chunks(), headers={"Content-Type": "application/json"}
    )
    assert response.status_code == 413


async def test_docs_hidden_in_production(app_factory: AppFactory, tmp_path) -> None:
    settings = make_settings(tmp_path, XERCES_ENV="production", DATABASE_URL="postgresql://u:p@db/x")
    app: FastAPI = await app_factory(settings=settings, schema=False)
    assert app.openapi_url is None
    assert app.docs_url is None
