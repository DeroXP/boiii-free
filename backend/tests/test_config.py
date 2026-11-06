"""Configuration: URL handling, production refusals, dev fallbacks, key formats and rotation."""

import json
import os
import subprocess
import sys
import uuid
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from pydantic import ValidationError
from sqlalchemy.engine import make_url

import app.main as main_module
from app.config import (
    ConfigError,
    Settings,
    hash_admin_token,
    load_private_key,
    normalize_database_url,
    resolve,
)
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


@pytest.mark.parametrize(
    "password",
    ["Sup3rS3cretPw?x", "p@ss:Sup3rS3cretPw", "a/b#c%40d", "with space", "plain"],
)
def test_special_characters_in_the_database_password(password: str) -> None:
    # Railway writes the password into its DATABASE_URL as it is; '%40' was already encoded.
    url = make_url(normalize_database_url(f"postgres://postgres:{password}@db.internal:5432/railway"))
    assert url.password == password.replace("%40", "@")
    assert (url.username, url.host, url.port, url.database) == ("postgres", "db.internal", 5432, "railway")


@pytest.mark.parametrize(
    "value",
    [
        "postgres://postgres:Sup3rS3cretPw@db.internal:Sup3rS3cretPw/railway",
        "postgres://postgres:Sup3rS3cretPw@[db.internal/railway",
    ],
)
def test_an_unusable_database_url_is_not_echoed(value: str) -> None:
    with pytest.raises(ConfigError) as error:
        normalize_database_url(value)
    message = str(error.value)
    assert "DATABASE_URL" in message
    assert "Sup3rS3cretPw" not in message and "db.internal" not in message
    assert error.value.__cause__ is None and error.value.__suppress_context__


def _environment(**values: str) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith(("XERCES_", "DATABASE_URL"))}
    env.update(PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8", **values)
    return env


@pytest.mark.parametrize(
    "command",
    [["-m", "alembic", "upgrade", "head"], ["-c", "from app.main import app"]],
    ids=["migrations", "server"],
)
def test_startup_with_an_unusable_database_url_prints_no_part_of_it(command: list[str]) -> None:
    # What start.sh runs first, and what uvicorn does when it loads the app.
    env = _environment(
        XERCES_ENV="production",
        DATABASE_URL="postgres://postgres:Sup3rS3cretPw@db.internal:Sup3rS3cretPw/railway",
        **dict(line.split("=", 1) for line in _genkey().splitlines()),
    )
    result = subprocess.run(
        [sys.executable, *command],
        cwd=BACKEND,
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
        stdin=subprocess.DEVNULL,
        check=False,
    )
    output = result.stdout + result.stderr
    assert result.returncode != 0
    assert "DATABASE_URL is not a valid database URL" in output
    assert "Sup3rS3cretPw" not in output


def test_engine_errors_name_the_error_type_only(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    def broken(_url: str) -> None:
        raise ValueError("invalid literal for int() with base 10: 'Sup3rS3cretPw'")

    monkeypatch.setattr(main_module, "create_engine", broken)
    with pytest.raises(ConfigError) as error:
        create_app(make_settings(tmp_path), tuning=make_tuning())
    assert str(error.value) == "DATABASE_URL could not be used (ValueError)."
    assert "Sup3rS3cretPw" not in caplog.text


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


def _genkey(*args: str, stdin: str | None = None) -> str:
    result = subprocess.run(
        [sys.executable, str(BACKEND / "scripts" / "genkey.py"), *args],
        input=stdin,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def test_genkey_output_is_a_valid_production_config() -> None:
    values = dict(line.split("=", 1) for line in _genkey().splitlines())
    assert set(values) == {"XERCES_TOKEN_PRIVATE_KEY", "XERCES_TOKEN_KEY_ID", "XERCES_RATE_LIMIT_SECRET"}
    config = resolve(
        _settings(
            XERCES_ENV="production",
            DATABASE_URL="postgresql://u:p@db/x",
            **values,
        )
    )
    assert config.key_id == values["XERCES_TOKEN_KEY_ID"]


def test_genkey_prints_the_public_key_of_a_seed_read_from_stdin() -> None:
    seed = new_seed()
    expected = load_private_key(seed).public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    ).decode().strip()
    # '-' reads the seed from stdin, so it never sits in shell history.
    assert _genkey("--public", "-", stdin=seed + "\n") == expected
    assert _genkey("--public", seed) == expected


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


def test_the_admin_token_is_optional_and_long_in_production() -> None:
    assert resolve(_settings(**_production())).admin_token_hash is None  # admin routes: 404
    token = "a" * 43
    config = resolve(_settings(**_production(XERCES_ADMIN_TOKEN=f"  {token}  ")))
    assert config.admin_token_hash == hash_admin_token(token)
    assert config.warnings == ()
    with pytest.raises(ConfigError, match="XERCES_ADMIN_TOKEN must be at least 32"):
        resolve(_settings(**_production(XERCES_ADMIN_TOKEN="short")))
    development = resolve(_settings(XERCES_ENV="development", XERCES_ADMIN_TOKEN="short"))
    assert development.admin_token_hash == hash_admin_token("short")
    assert any("XERCES_ADMIN_TOKEN" in warning for warning in development.warnings)


def test_the_admin_token_never_shows_in_errors_or_reprs() -> None:
    token = "secret-admin-token-" + "z" * 30
    settings = _settings(**_production(XERCES_ADMIN_TOKEN=token))
    assert token not in repr(settings)
    assert token not in repr(resolve(settings))


def test_genkey_prints_an_admin_token() -> None:
    line = _genkey("--admin")
    name, _, token = line.partition("=")
    assert name == "XERCES_ADMIN_TOKEN"
    assert len(token) >= 43
    config = resolve(_settings(**_production(XERCES_ADMIN_TOKEN=token)))
    assert config.admin_token_hash == hash_admin_token(token)
    assert _genkey("--admin") != line
