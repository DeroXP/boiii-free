"""Configuration: URL handling, production refusals, dev fallbacks, key formats and rotation."""

import json
import subprocess
import sys
import uuid
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from pydantic import ValidationError

from app.config import ConfigError, Settings, load_private_key, normalize_database_url, resolve
from app.main import create_app
from app.tokens import AccessTokens
from tests.helpers import (
    AppFactory,
    client_for,
    make_settings,
    make_tuning,
    new_seed,
    register,
)

BACKEND = Path(__file__).resolve().parents[1]


def _settings(**values: object) -> Settings:
    return Settings(_env_file=None, **values)


def _production(**overrides: object) -> dict[str, object]:
    values: dict[str, object] = {
        "XERCES_ENV": "production",
        "DATABASE_URL": "postgres://user:secret@db.internal:5432/railway",
        "XERCES_TOKEN_PRIVATE_KEY": new_seed(),
        "XERCES_TOKEN_KEY_ID": "2026-11-04-abcd",
        "XERCES_RATE_LIMIT_SECRET": "s" * 43,
    }
    values.update(overrides)
    return values


@pytest.mark.parametrize(
    ("given", "expected"),
    [
        ("postgres://u:p@host:5432/db", "postgresql+asyncpg://u:p@host:5432/db"),
        ("postgresql://u:p@host/db", "postgresql+asyncpg://u:p@host/db"),
        ("postgresql+asyncpg://u:p@host/db", "postgresql+asyncpg://u:p@host/db"),
        ("postgresql://u:p@host/db?sslmode=require", "postgresql+asyncpg://u:p@host/db?ssl=require"),
        ("sqlite:///./x.db", "sqlite+aiosqlite:///./x.db"),
        ("  postgresql://u:p@host/db  ", "postgresql+asyncpg://u:p@host/db"),
    ],
)
def test_database_url_conversion(given: str, expected: str) -> None:
    assert normalize_database_url(given) == expected


def test_unsupported_database_url_does_not_echo_the_password() -> None:
    with pytest.raises(ConfigError) as error:
        normalize_database_url("mysql://user:hunter2@host/db")
    assert "hunter2" not in str(error.value)


def test_production_accepts_a_complete_environment() -> None:
    config = resolve(_settings(**_production()))
    assert config.env == "production"
    assert config.database_url.startswith("postgresql+asyncpg://")
    assert config.warnings == ()


@pytest.mark.parametrize(
    ("missing", "fragment"),
    [
        ("XERCES_TOKEN_PRIVATE_KEY", "XERCES_TOKEN_PRIVATE_KEY"),
        ("XERCES_TOKEN_KEY_ID", "XERCES_TOKEN_KEY_ID"),
        ("XERCES_RATE_LIMIT_SECRET", "XERCES_RATE_LIMIT_SECRET"),
        ("DATABASE_URL", "DATABASE_URL"),
    ],
)
def test_production_refuses_missing_values(missing: str, fragment: str) -> None:
    values = _production()
    del values[missing]
    with pytest.raises(ConfigError, match=fragment):
        resolve(_settings(**values))


def test_production_refuses_sqlite_and_weak_secret() -> None:
    with pytest.raises(ConfigError, match="SQLite"):
        resolve(_settings(**_production(DATABASE_URL="sqlite+aiosqlite:///./x.db")))
    with pytest.raises(ConfigError, match="at least 32"):
        resolve(_settings(**_production(XERCES_RATE_LIMIT_SECRET="short")))


def test_proxy_hops() -> None:
    assert resolve(_settings(**_production())).proxy_hops == 0  # the first X-Forwarded-For entry
    assert resolve(_settings(**_production(XERCES_PROXY_HOPS="1"))).proxy_hops == 1
    for bad in ("-1", "11", "last"):
        with pytest.raises(ValidationError):
            _settings(**_production(XERCES_PROXY_HOPS=bad))


def test_production_is_the_default_env() -> None:
    with pytest.raises(ConfigError):
        resolve(_settings())


def test_development_generates_throwaway_secrets() -> None:
    config = resolve(_settings(XERCES_ENV="Development"))
    assert config.env == "development"
    assert config.database_url.startswith("sqlite+aiosqlite")
    assert config.key_id == "dev-ephemeral"
    assert len(config.rate_limit_secret) == 32
    assert any("throwaway signing key" in warning for warning in config.warnings)


def test_bad_keys_are_rejected() -> None:
    for value in ("not-a-key", "AAAA", "-----BEGIN PRIVATE KEY-----\ngarbage\n-----END PRIVATE KEY-----"):
        with pytest.raises(ConfigError):
            resolve(_settings(**_production(XERCES_TOKEN_PRIVATE_KEY=value)))
    with pytest.raises(ConfigError):
        resolve(_settings(**_production(XERCES_TOKEN_KEY_ID="has spaces")))


def test_pem_private_key_and_escaped_newlines() -> None:
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ).decode()
    for value in (pem, pem.replace("\n", "\\n")):
        config = resolve(_settings(**_production(XERCES_TOKEN_PRIVATE_KEY=value)))
        assert config.signing_key.public_key().public_bytes_raw() == key.public_key().public_bytes_raw()


def test_rotation_with_extra_public_keys() -> None:
    old = Ed25519PrivateKey.generate()
    old_pem = old.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    ).decode()
    old_tokens = AccessTokens(old, "old-key", {"old-key": old.public_key()})
    user_id = uuid.uuid4()
    token = old_tokens.issue(user_id)

    config = resolve(
        _settings(**_production(XERCES_TOKEN_EXTRA_PUBLIC_KEYS=json.dumps({"old-key": old_pem})))
    )
    new_tokens = AccessTokens(config.signing_key, config.key_id, config.verify_keys)
    assert new_tokens.verify(token) == user_id  # still accepted during rotation
    assert new_tokens.verify(new_tokens.issue(user_id)) == user_id

    with pytest.raises(ConfigError):
        resolve(_settings(**_production(XERCES_TOKEN_EXTRA_PUBLIC_KEYS="[1, 2]")))
    with pytest.raises(ConfigError):
        resolve(
            _settings(
                **_production(
                    XERCES_TOKEN_KEY_ID="same",
                    XERCES_TOKEN_EXTRA_PUBLIC_KEYS=json.dumps({"same": old_pem}),
                )
            )
        )


async def test_app_refuses_to_start_in_production_without_secrets(tmp_path: Path) -> None:
    settings = make_settings(tmp_path, XERCES_ENV="production", XERCES_TOKEN_PRIVATE_KEY=None)
    with pytest.raises(ConfigError, match="Refusing to start"):
        create_app(settings, tuning=make_tuning())


def test_genkey_output_is_a_valid_production_config() -> None:
    result = subprocess.run(
        [sys.executable, str(BACKEND / "scripts" / "genkey.py")],
        capture_output=True,
        text=True,
        check=True,
    )
    values = dict(line.split("=", 1) for line in result.stdout.strip().splitlines())
    assert set(values) == {"XERCES_TOKEN_PRIVATE_KEY", "XERCES_TOKEN_KEY_ID", "XERCES_RATE_LIMIT_SECRET"}
    config = resolve(
        _settings(
            XERCES_ENV="production",
            DATABASE_URL="postgresql://u:p@db/x",
            **values,
        )
    )
    assert config.key_id == values["XERCES_TOKEN_KEY_ID"]


async def test_old_tokens_survive_key_rotation(app_factory: AppFactory, tmp_path: Path) -> None:
    first = make_settings(tmp_path)
    app = await app_factory(settings=first)
    async with client_for(app) as client:
        user = await register(client, "rotator")

    old_seed = first.token_private_key.get_secret_value()  # type: ignore[union-attr]
    old_public = load_private_key(old_seed).public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    ).decode()
    rotated = make_settings(
        tmp_path,
        XERCES_TOKEN_KEY_ID="next-key",
        XERCES_TOKEN_EXTRA_PUBLIC_KEYS=json.dumps({"test-key": old_public}),
    )
    app2 = await app_factory(settings=rotated, schema=False)
    async with client_for(app2) as client:
        response = await client.get("/v1/me", headers=user.headers)
        assert response.status_code == 200
        # Refresh tokens are not signed, so they keep working across a key change too.
        refreshed = await client.post("/v1/auth/refresh", json={"refresh_token": user.refresh_token})
        assert refreshed.status_code == 200
    unrelated = make_settings(tmp_path, XERCES_TOKEN_KEY_ID="other-key")
    app3 = await app_factory(settings=unrelated, schema=False)
    async with client_for(app3) as client:
        assert (await client.get("/v1/me", headers=user.headers)).status_code == 401
