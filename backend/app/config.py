"""Environment configuration.

`Settings` declares every environment variable. `resolve()` turns it into what the app runs with:
production refuses to start when a secret is missing or the database is SQLite; development and
test fall back to throwaway secrets and say so loudly.
"""

import base64
import binascii
import hashlib
import json
import re
import secrets
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, Literal
from urllib.parse import parse_qsl, quote, urlencode, urlsplit, urlunsplit

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)
from pydantic import Field, SecretStr, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict
from sqlalchemy.engine import make_url
from sqlalchemy.exc import ArgumentError

from app.ratelimit import Rule

DEV_DATABASE_URL = "sqlite+aiosqlite:///./xerces-dev.db"
# OWASP Password Storage Cheat Sheet minimum for Argon2id.
OWASP_MIN_TIME_COST = 2
OWASP_MIN_MEMORY_KIB = 19456
MIN_RATE_LIMIT_SECRET_CHARS = 32
MIN_ADMIN_TOKEN_CHARS = 32
KEY_ID_PATTERN = re.compile(r"[A-Za-z0-9._-]{1,64}")
_POSTGRES_SCHEMES = frozenset({"postgres", "postgresql", "postgresql+asyncpg"})
_SQLITE_SCHEMES = frozenset({"sqlite", "sqlite+aiosqlite"})


class ConfigError(RuntimeError):
    """The environment is incomplete or unsafe, so the service must not start."""


class Settings(BaseSettings):
    """Environment variables (the aliases are the variable names)."""

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        env_ignore_empty=True,
        extra="ignore",
    )

    database_url: str | None = Field(default=None, validation_alias="DATABASE_URL")
    env: Literal["production", "development", "test"] = Field(
        default="production", validation_alias="XERCES_ENV"
    )
    token_private_key: SecretStr | None = Field(
        default=None, validation_alias="XERCES_TOKEN_PRIVATE_KEY"
    )
    token_key_id: str | None = Field(default=None, validation_alias="XERCES_TOKEN_KEY_ID")
    token_extra_public_keys: str | None = Field(
        default=None, validation_alias="XERCES_TOKEN_EXTRA_PUBLIC_KEYS"
    )
    rate_limit_secret: SecretStr | None = Field(
        default=None, validation_alias="XERCES_RATE_LIMIT_SECRET"
    )
    # Bearer token for the /v1/admin routes (crash reports). Unset: those routes answer 404.
    admin_token: SecretStr | None = Field(default=None, validation_alias="XERCES_ADMIN_TOKEN")
    # What all crash reports together may take in the database; over it new ones get 429. The
    # rest of the database (accounts, friends) must never run out of room because of them.
    crash_storage_mb: int = Field(
        default=1024, ge=1, le=1024 * 1024, validation_alias="XERCES_CRASH_STORAGE_MB"
    )
    trust_proxy: bool = Field(default=False, validation_alias="XERCES_TRUST_PROXY")
    # Which X-Forwarded-For entry is the client (used only with XERCES_TRUST_PROXY=1): 0 takes the
    # first entry, for an edge that replaces the header; N takes the Nth entry from the right, for
    # N proxies that each append to it.
    proxy_hops: int = Field(default=0, ge=0, le=10, validation_alias="XERCES_PROXY_HOPS")
    argon2_time_cost: int = Field(
        default=2, ge=1, le=50, validation_alias="XERCES_ARGON2_TIME_COST"
    )
    argon2_memory_kib: int = Field(
        default=19456, ge=8, le=4 * 1024 * 1024, validation_alias="XERCES_ARGON2_MEMORY_KIB"
    )
    argon2_parallelism: int = Field(
        default=1, ge=1, le=16, validation_alias="XERCES_ARGON2_PARALLELISM"
    )
    # Read by start.sh (uvicorn --port); kept here so every variable is documented in one place.
    port: int = Field(default=8080, ge=1, le=65535, validation_alias="PORT")

    @field_validator("env", mode="before")
    @classmethod
    def _normalize_env(cls, value: Any) -> Any:
        return value.strip().lower() if isinstance(value, str) else value


@dataclass(frozen=True)
class Tuning:
    """Knobs that are not environment variables. Tests shorten timeouts and loosen limits."""

    rate_rules: Mapping[str, Rule] | None = None
    cleanup_interval: float | None = 600.0
    ws_auth_timeout: float = 10.0
    max_body_bytes: int = 128 * 1024
    hash_concurrency: int = 4
    # Requests that may wait for a hash slot; more are turned away with 503 (app.passwords).
    hash_queue: int = 64
    configure_logging: bool = True
    # POST /v1/crashes: the largest body (before and after gzip decoding), how many such uploads
    # may be read and handled at once, in all and from one address (the rest get 503), and how
    # long reading one may take. One upload costs at most about 50 MB of memory while handled.
    crash_max_body_bytes: int = 4 * 1024 * 1024
    crash_upload_slots: int = 4
    crash_upload_slots_per_address: int = 2
    crash_body_timeout: float = 30.0


@dataclass(frozen=True)
class RuntimeConfig:
    env: str
    database_url: str
    signing_key: Ed25519PrivateKey
    key_id: str
    verify_keys: Mapping[str, Ed25519PublicKey]
    rate_limit_secret: bytes
    trust_proxy: bool
    argon2_time_cost: int
    argon2_memory_kib: int
    argon2_parallelism: int
    proxy_hops: int = 0
    # SHA-256 of XERCES_ADMIN_TOKEN, compared in constant time (app.routers.admin); None when unset.
    admin_token_hash: bytes | None = None
    crash_storage_bytes: int = 1024 * 1024 * 1024
    warnings: tuple[str, ...] = ()


def normalize_database_url(url: str) -> str:
    """Return a SQLAlchemy async URL. Railway hands out postgres:// or postgresql:// URLs.

    Railway writes the password into its URL as it is, so the user name and password (everything
    before the last `@`) are percent-encoded here: `@`, `:`, `/`, `?` and `#` in a password work.
    A `%` is taken as the start of an escape, as in any URL. No error quotes the URL, because the
    rest of it holds the password (SQLAlchemy's parse errors do quote it).
    """
    value = url.strip()
    scheme, separator, rest = value.partition("://")
    if not separator or not rest:
        raise ConfigError(
            "DATABASE_URL must look like postgresql://user:password@host:5432/database."
        )
    scheme = scheme.lower()
    if scheme not in _POSTGRES_SCHEMES | _SQLITE_SCHEMES:
        # Only the scheme is echoed.
        raise ConfigError(
            f"DATABASE_URL scheme {scheme!r} is not supported; use postgresql:// "
            "(or sqlite+aiosqlite:// for local tests)."
        )
    try:
        if scheme in _SQLITE_SCHEMES:
            normalized = "sqlite+aiosqlite://" + rest
        else:
            normalized = _asyncpg_query("postgresql+asyncpg://" + _quote_userinfo(rest))
        make_url(normalized)
    except (ArgumentError, ValueError):
        raise ConfigError(
            "DATABASE_URL is not a valid database URL; check its host, port and database name."
        ) from None
    return normalized


def _quote_userinfo(rest: str) -> str:
    userinfo, at, location = rest.rpartition("@")
    if not at:
        return rest
    user, colon, password = userinfo.partition(":")
    return quote(user, safe="%") + colon + quote(password, safe="%") + at + location


def _asyncpg_query(url: str) -> str:
    """asyncpg takes `ssl`, not libpq's `sslmode`."""
    parts = urlsplit(url)
    if not parts.query:
        return url
    pairs = parse_qsl(parts.query, keep_blank_values=True)
    has_ssl = any(key == "ssl" for key, _ in pairs)
    query = []
    for key, value in pairs:
        if key == "sslmode":
            if not has_ssl:
                query.append(("ssl", value))
            continue
        query.append((key, value))
    return urlunsplit(parts._replace(query=urlencode(query)))


def _b64decode(value: str) -> bytes:
    text = value.strip().replace("+", "-").replace("/", "_").rstrip("=")
    text += "=" * (-len(text) % 4)
    return base64.b64decode(text, altchars=b"-_", validate=True)


def load_private_key(value: str) -> Ed25519PrivateKey:
    """Accept a PEM Ed25519 private key or a base64url 32-byte seed (what genkey.py prints)."""
    text = value.strip()
    if text.startswith("-----BEGIN"):
        try:
            key = serialization.load_pem_private_key(
                text.replace("\\n", "\n").encode("ascii"), password=None
            )
        except (ValueError, TypeError, UnicodeEncodeError):
            raise ConfigError(
                "XERCES_TOKEN_PRIVATE_KEY is not a readable, unencrypted PEM private key."
            ) from None
        if not isinstance(key, Ed25519PrivateKey):
            raise ConfigError("XERCES_TOKEN_PRIVATE_KEY must be an Ed25519 key.")
        return key
    try:
        seed = _b64decode(text)
    except (binascii.Error, ValueError):
        seed = b""
    if len(seed) != 32:
        raise ConfigError(
            "XERCES_TOKEN_PRIVATE_KEY must be a PEM Ed25519 private key or a base64url "
            "32-byte seed (run scripts/genkey.py)."
        )
    return Ed25519PrivateKey.from_private_bytes(seed)


def load_public_key(value: str, kid: str) -> Ed25519PublicKey:
    """Accept a PEM Ed25519 public key or its raw 32 bytes in base64url."""
    text = value.strip()
    problem = f"XERCES_TOKEN_EXTRA_PUBLIC_KEYS[{kid!r}] must be an Ed25519 public key."
    if text.startswith("-----BEGIN"):
        try:
            key = serialization.load_pem_public_key(text.replace("\\n", "\n").encode("ascii"))
        except (ValueError, TypeError, UnicodeEncodeError):
            raise ConfigError(problem) from None
        if not isinstance(key, Ed25519PublicKey):
            raise ConfigError(problem)
        return key
    try:
        raw = _b64decode(text)
    except (binascii.Error, ValueError):
        raw = b""
    if len(raw) != 32:
        raise ConfigError(problem)
    return Ed25519PublicKey.from_public_bytes(raw)


def load_extra_public_keys(raw: str | None, current_kid: str) -> dict[str, Ed25519PublicKey]:
    if not raw:
        return {}
    try:
        data = json.loads(raw)
    except ValueError:
        data = None
    if not isinstance(data, dict):
        raise ConfigError(
            'XERCES_TOKEN_EXTRA_PUBLIC_KEYS must be a JSON object like {"old-kid": "-----BEGIN '
            'PUBLIC KEY-----..."}.'
        )
    keys: dict[str, Ed25519PublicKey] = {}
    for kid, value in data.items():
        if not KEY_ID_PATTERN.fullmatch(kid) or not isinstance(value, str):
            raise ConfigError("XERCES_TOKEN_EXTRA_PUBLIC_KEYS has a bad key id or value.")
        if kid == current_kid:
            raise ConfigError(
                "XERCES_TOKEN_EXTRA_PUBLIC_KEYS must not repeat XERCES_TOKEN_KEY_ID."
            )
        keys[kid] = load_public_key(value, kid)
    return keys


def hash_admin_token(token: str) -> bytes:
    """What an admin token is compared as: equal-length digests, so the compare leaks nothing."""
    return hashlib.sha256(token.encode("utf-8")).digest()


def resolve_database_url(settings: Settings) -> str:
    """The async database URL; also used by migrations/env.py, which needs nothing else."""
    production = settings.env == "production"
    if settings.database_url is None:
        if production:
            raise ConfigError(
                "DATABASE_URL is not set (use the Railway PostgreSQL reference variable)."
            )
        return DEV_DATABASE_URL
    database_url = normalize_database_url(settings.database_url)
    if production and database_url.startswith("sqlite"):
        raise ConfigError("DATABASE_URL points at SQLite; production needs PostgreSQL.")
    return database_url


def resolve(settings: Settings) -> RuntimeConfig:
    production = settings.env == "production"
    problems: list[str] = []
    warnings: list[str] = []

    try:
        database_url = resolve_database_url(settings)
    except ConfigError as exc:
        if not production:
            raise
        problems.append(str(exc))
        database_url = ""

    signing_key = None
    if settings.token_private_key is not None:
        signing_key = load_private_key(settings.token_private_key.get_secret_value())
    elif production:
        problems.append("XERCES_TOKEN_PRIVATE_KEY is not set (run scripts/genkey.py).")

    key_id = settings.token_key_id
    if key_id is not None and not KEY_ID_PATTERN.fullmatch(key_id):
        raise ConfigError(
            "XERCES_TOKEN_KEY_ID may only use letters, digits, '.', '_' and '-' (at most 64)."
        )
    if key_id is None and production:
        problems.append("XERCES_TOKEN_KEY_ID is not set (run scripts/genkey.py).")

    secret = settings.rate_limit_secret.get_secret_value() if settings.rate_limit_secret else None
    if secret is None and production:
        problems.append("XERCES_RATE_LIMIT_SECRET is not set (run scripts/genkey.py).")
    elif secret is not None and production and len(secret) < MIN_RATE_LIMIT_SECRET_CHARS:
        problems.append(
            f"XERCES_RATE_LIMIT_SECRET must be at least {MIN_RATE_LIMIT_SECRET_CHARS} characters."
        )

    admin_token = settings.admin_token.get_secret_value().strip() if settings.admin_token else ""
    if admin_token and len(admin_token) < MIN_ADMIN_TOKEN_CHARS:
        message = (
            f"XERCES_ADMIN_TOKEN must be at least {MIN_ADMIN_TOKEN_CHARS} characters "
            "(run scripts/genkey.py --admin)."
        )
        if production:
            problems.append(message)
        else:
            warnings.append(message)

    if settings.argon2_memory_kib < 8 * settings.argon2_parallelism:
        raise ConfigError(
            "XERCES_ARGON2_MEMORY_KIB must be at least 8 times XERCES_ARGON2_PARALLELISM."
        )

    if problems:
        raise ConfigError("Refusing to start in production. " + " ".join(problems))

    if signing_key is None:
        signing_key = Ed25519PrivateKey.generate()
        key_id = key_id or "dev-ephemeral"
        warnings.append(
            "XERCES_TOKEN_PRIVATE_KEY is not set: using a throwaway signing key, so every access "
            "token dies with this process. Never run production like this."
        )
    if key_id is None:
        key_id = "dev"
    if secret is None:
        rate_limit_secret = secrets.token_bytes(32)
        warnings.append("XERCES_RATE_LIMIT_SECRET is not set: using a throwaway secret.")
    else:
        rate_limit_secret = secret.encode("utf-8")
    if production and (
        settings.argon2_time_cost < OWASP_MIN_TIME_COST
        or settings.argon2_memory_kib < OWASP_MIN_MEMORY_KIB
    ):
        warnings.append(
            "Argon2 parameters are below the OWASP minimum (2 iterations, 19456 KiB, 1 lane)."
        )

    verify_keys: dict[str, Ed25519PublicKey] = {key_id: signing_key.public_key()}
    verify_keys.update(load_extra_public_keys(settings.token_extra_public_keys, key_id))

    return RuntimeConfig(
        env=settings.env,
        database_url=database_url,
        signing_key=signing_key,
        key_id=key_id,
        verify_keys=verify_keys,
        rate_limit_secret=rate_limit_secret,
        trust_proxy=settings.trust_proxy,
        argon2_time_cost=settings.argon2_time_cost,
        argon2_memory_kib=settings.argon2_memory_kib,
        argon2_parallelism=settings.argon2_parallelism,
        proxy_hops=settings.proxy_hops,
        admin_token_hash=hash_admin_token(admin_token) if admin_token else None,
        crash_storage_bytes=settings.crash_storage_mb * 1024 * 1024,
        warnings=tuple(warnings),
    )
