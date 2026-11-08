"""Database tables. Types are portable: the same models run on PostgreSQL and on SQLite (tests).

Every timestamp is a timezone-aware UTC datetime in Python. Every foreign key to users cascades on
delete, except reports, which keep the row and lose the link (see migrations/versions).
"""

import uuid
from datetime import UTC, datetime
from typing import Any

from sqlalchemy import (
    JSON,
    BigInteger,
    CheckConstraint,
    DateTime,
    ForeignKey,
    Integer,
    LargeBinary,
    MetaData,
    String,
    TypeDecorator,
    UniqueConstraint,
    Uuid,
    false,
)
from sqlalchemy.engine import Dialect
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column

NAMING_CONVENTION = {
    "ix": "ix_%(column_0_label)s",
    "uq": "uq_%(table_name)s_%(column_0_N_name)s",
    "ck": "ck_%(table_name)s_%(constraint_name)s",
    "fk": "fk_%(table_name)s_%(column_0_name)s_%(referred_table_name)s",
    "pk": "pk_%(table_name)s",
}

PRIVACY_FRIEND_REQUESTS = ("everyone", "nobody")
PRIVACY_PRESENCE = ("friends", "nobody")
PRIVACY_INVITES = ("friends", "nobody")
REPORT_REASONS = ("spam", "harassment", "cheating", "impersonation", "other")
REPORT_STATUSES = ("open", "closed")
INVITE_STATUSES = ("pending", "accepted", "declined")
KNOCK_STATUSES = ("pending", "accepted", "declined")
GAME_MODES = ("zm", "mp", "cp")
CRASH_KINDS = ("exception", "fail_fast", "out_of_memory", "unknown_exit")


def utcnow() -> datetime:
    return datetime.now(UTC)


def iso(value: datetime | None) -> str | None:
    """ISO 8601 in UTC with a Z suffix, the same form Pydantic uses in responses."""
    if value is None:
        return None
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _in(column: str, values: tuple[str, ...]) -> str:
    return f"{column} IN ({', '.join(repr(v) for v in values)})"


class UTCDateTime(TypeDecorator[datetime]):
    """Stores UTC; always hands back aware UTC datetimes (SQLite drops the offset)."""

    impl = DateTime(timezone=True)
    cache_ok = True

    def process_bind_param(self, value: datetime | None, dialect: Dialect) -> datetime | None:
        if value is None:
            return None
        if value.tzinfo is None:
            raise ValueError("naive datetimes are not stored; use app.models.utcnow()")
        value = value.astimezone(UTC)
        return value.replace(tzinfo=None) if dialect.name == "sqlite" else value

    def process_result_value(self, value: datetime | None, dialect: Dialect) -> datetime | None:
        if value is None:
            return None
        if value.tzinfo is None:
            return value.replace(tzinfo=UTC)
        return value.astimezone(UTC)


class Base(DeclarativeBase):
    metadata = MetaData(naming_convention=NAMING_CONVENTION)
    type_annotation_map = {datetime: UTCDateTime(), uuid.UUID: Uuid()}


def _user_fk(ondelete: str = "CASCADE") -> ForeignKey:
    return ForeignKey("users.id", ondelete=ondelete)


class User(Base):
    __tablename__ = "users"
    __table_args__ = (
        CheckConstraint(
            _in("privacy_friend_requests", PRIVACY_FRIEND_REQUESTS),
            name="privacy_friend_requests",
        ),
        CheckConstraint(_in("privacy_presence", PRIVACY_PRESENCE), name="privacy_presence"),
        CheckConstraint(_in("privacy_invites", PRIVACY_INVITES), name="privacy_invites"),
    )

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    username: Mapped[str] = mapped_column(String(24))
    username_lower: Mapped[str] = mapped_column(String(24), unique=True)
    password_hash: Mapped[str] = mapped_column(String(255))
    email: Mapped[str | None] = mapped_column(String(254))
    privacy_friend_requests: Mapped[str] = mapped_column(
        String(16), default="everyone", server_default="everyone"
    )
    privacy_presence: Mapped[str] = mapped_column(
        String(16), default="friends", server_default="friends"
    )
    privacy_invites: Mapped[str] = mapped_column(
        String(16), default="friends", server_default="friends"
    )
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    # Set on password change: access tokens issued before it no longer work (see app.tokens).
    tokens_valid_after: Mapped[datetime | None]


class DeletedUsername(Base):
    """The name of a deleted account, held for 90 days so nobody can take it over and pass as its
    former owner. Only the name is kept, with no link to the account that had it."""

    __tablename__ = "deleted_usernames"

    username_lower: Mapped[str] = mapped_column(String(24), primary_key=True)
    expires_at: Mapped[datetime] = mapped_column(index=True)


class RefreshToken(Base):
    __tablename__ = "refresh_tokens"

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    user_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    # SHA-256 hex of the token; the token itself is never stored.
    token_hash: Mapped[str] = mapped_column(String(64), unique=True)
    family_id: Mapped[uuid.UUID] = mapped_column(index=True)
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    expires_at: Mapped[datetime] = mapped_column(index=True)
    revoked_at: Mapped[datetime | None]
    replaced_by: Mapped[uuid.UUID | None]


class FriendRequest(Base):
    __tablename__ = "friend_requests"
    __table_args__ = (
        UniqueConstraint("from_user_id", "to_user_id"),
        CheckConstraint("from_user_id <> to_user_id", name="not_self"),
    )

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    from_user_id: Mapped[uuid.UUID] = mapped_column(_user_fk())
    to_user_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    expires_at: Mapped[datetime] = mapped_column(index=True)
    # Shown to the sender only: the recipient had blocked the sender or accepted no requests when
    # it was sent. Keeping it lets the sender's outgoing list look the same either way.
    hidden: Mapped[bool] = mapped_column(default=False, server_default=false())


class Friendship(Base):
    """One row per pair, ids ordered so (a, b) and (b, a) cannot both exist."""

    __tablename__ = "friendships"
    __table_args__ = (CheckConstraint("user_low < user_high", name="ordered_pair"),)

    user_low: Mapped[uuid.UUID] = mapped_column(_user_fk(), primary_key=True)
    user_high: Mapped[uuid.UUID] = mapped_column(_user_fk(), primary_key=True, index=True)
    created_at: Mapped[datetime] = mapped_column(default=utcnow)


class Block(Base):
    __tablename__ = "blocks"
    __table_args__ = (CheckConstraint("blocker_id <> blocked_id", name="not_self"),)

    blocker_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), primary_key=True)
    blocked_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), primary_key=True, index=True)
    created_at: Mapped[datetime] = mapped_column(default=utcnow)


class Report(Base):
    __tablename__ = "reports"
    __table_args__ = (
        CheckConstraint(_in("reason", REPORT_REASONS), name="reason"),
        CheckConstraint(_in("status", REPORT_STATUSES), name="status"),
    )

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    reporter_id: Mapped[uuid.UUID | None] = mapped_column(_user_fk("SET NULL"), index=True)
    reported_id: Mapped[uuid.UUID | None] = mapped_column(_user_fk("SET NULL"), index=True)
    reason: Mapped[str] = mapped_column(String(16))
    details: Mapped[str | None] = mapped_column(String(500))
    created_at: Mapped[datetime] = mapped_column(default=utcnow, index=True)
    status: Mapped[str] = mapped_column(String(8), default="open", server_default="open")


class Invite(Base):
    __tablename__ = "invites"
    __table_args__ = (
        CheckConstraint(_in("status", INVITE_STATUSES), name="status"),
        CheckConstraint("from_user_id <> to_user_id", name="not_self"),
    )

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    from_user_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    to_user_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    session: Mapped[str] = mapped_column(String(256))
    pack_code: Mapped[str | None] = mapped_column(String(12))
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    expires_at: Mapped[datetime] = mapped_column(index=True)
    status: Mapped[str] = mapped_column(String(10), default="pending", server_default="pending")


class InviteLink(Base):
    """A host's link that asks to join their game (DESIGN.md 10.12): XJN- plus 8 Crockford base32
    characters, kept a day. A new one replaces the host's old ones."""

    __tablename__ = "invite_links"
    __table_args__ = (CheckConstraint(_in("mode", GAME_MODES), name="mode"),)

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    code: Mapped[str] = mapped_column(String(12), unique=True)
    owner_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    mode: Mapped[str] = mapped_column(String(2))
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    expires_at: Mapped[datetime] = mapped_column(index=True)


class Knock(Base):
    """A player who followed a host's link and asks to join. Pending for 10 minutes; once the
    host accepts, the host may invite them for an hour although they are not friends."""

    __tablename__ = "knocks"
    __table_args__ = (
        CheckConstraint(_in("status", KNOCK_STATUSES), name="status"),
        CheckConstraint(_in("mode", GAME_MODES), name="mode"),
        CheckConstraint("from_user_id <> to_user_id", name="not_self"),
    )

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    from_user_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    to_user_id: Mapped[uuid.UUID] = mapped_column(_user_fk(), index=True)
    # The link's mode when they knocked, so the host sees what they asked to join.
    mode: Mapped[str] = mapped_column(String(2))
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    expires_at: Mapped[datetime] = mapped_column(index=True)
    status: Mapped[str] = mapped_column(String(10), default="pending", server_default="pending")


class Pack(Base):
    __tablename__ = "packs"

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    code: Mapped[str] = mapped_column(String(12), unique=True)
    owner_id: Mapped[uuid.UUID | None] = mapped_column(_user_fk(), index=True)
    data: Mapped[dict[str, Any]] = mapped_column(JSON)
    created_at: Mapped[datetime] = mapped_column(default=utcnow)
    expires_at: Mapped[datetime] = mapped_column(index=True)


class CrashReport(Base):
    """A crash report sent by a launcher (POST /v1/crashes), kept 90 days.

    Keyed to a random install id, never to an account. No client address is stored. The column
    names follow the report's fields; `mods`, `system`, `crash` and `logs` hold those parts as
    sent, after the server's path scrub (app.crashes).
    """

    __tablename__ = "crash_reports"
    __table_args__ = (CheckConstraint(_in("kind", CRASH_KINDS), name="kind"),)

    id: Mapped[uuid.UUID] = mapped_column(primary_key=True, default=uuid.uuid4)
    # The client's own id for the report, so a resend is recognized (idempotency key).
    report_id: Mapped[str] = mapped_column(String(32), unique=True)
    install_id: Mapped[str] = mapped_column(String(32), index=True)
    created_utc: Mapped[datetime]
    received_at: Mapped[datetime] = mapped_column(default=utcnow, index=True)
    version: Mapped[str] = mapped_column(String(64), index=True)
    commit: Mapped[str | None] = mapped_column(String(64))
    dirty: Mapped[bool | None]
    game_build: Mapped[str | None] = mapped_column(String(64))
    steam_build: Mapped[int | None] = mapped_column(BigInteger)
    kind: Mapped[str] = mapped_column(String(16))
    code: Mapped[str | None] = mapped_column(String(18))
    module: Mapped[str | None] = mapped_column(String(256))
    offset: Mapped[str | None] = mapped_column(String(18))
    signature: Mapped[str] = mapped_column(String(16), index=True)
    phase: Mapped[str | None] = mapped_column(String(64))
    mode: Mapped[str | None] = mapped_column(String(2))
    map: Mapped[str | None] = mapped_column(String(256))
    plan_id: Mapped[str | None] = mapped_column(String(256))
    safe_mode: Mapped[bool | None]
    flags: Mapped[list[str] | None] = mapped_column(JSON)
    mods: Mapped[list[dict[str, Any]] | None] = mapped_column(JSON)
    system: Mapped[dict[str, Any] | None] = mapped_column(JSON)
    crash: Mapped[dict[str, Any]] = mapped_column(JSON)
    logs: Mapped[dict[str, Any] | None] = mapped_column(JSON)
    # About what the report takes in the database, minidump included (app.crashes.stored_size).
    # Their sum is held under XERCES_CRASH_STORAGE_MB.
    stored_bytes: Mapped[int] = mapped_column(Integer)


class CrashMinidump(Base):
    """The stacks-only minidump of a report, in its own table so lists never load it."""

    __tablename__ = "crash_minidumps"

    crash_id: Mapped[uuid.UUID] = mapped_column(
        ForeignKey("crash_reports.id", ondelete="CASCADE"), primary_key=True
    )
    size: Mapped[int] = mapped_column(Integer)
    data: Mapped[bytes] = mapped_column(LargeBinary)
