"""Initial schema: users, refresh tokens, friends, blocks, reports, invites and packs.

Revision ID: 0001
Revises:
Create Date: 2026-11-04
"""

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0001"
down_revision: str | Sequence[str] | None = None
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamp(name: str, nullable: bool = False) -> sa.Column:
    return sa.Column(name, sa.DateTime(timezone=True), nullable=nullable)


def _user_fk(column: str, table: str, ondelete: str = "CASCADE") -> sa.ForeignKeyConstraint:
    return sa.ForeignKeyConstraint(
        [column], ["users.id"], name=op.f(f"fk_{table}_{column}_users"), ondelete=ondelete
    )


def upgrade() -> None:
    op.create_table(
        "users",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("username", sa.String(length=24), nullable=False),
        sa.Column("username_lower", sa.String(length=24), nullable=False),
        sa.Column("password_hash", sa.String(length=255), nullable=False),
        sa.Column("email", sa.String(length=254), nullable=True),
        sa.Column(
            "privacy_friend_requests", sa.String(length=16), server_default="everyone", nullable=False
        ),
        sa.Column("privacy_presence", sa.String(length=16), server_default="friends", nullable=False),
        sa.Column("privacy_invites", sa.String(length=16), server_default="friends", nullable=False),
        _timestamp("created_at"),
        sa.CheckConstraint(
            "privacy_friend_requests IN ('everyone', 'nobody')",
            name=op.f("ck_users_privacy_friend_requests"),
        ),
        sa.CheckConstraint(
            "privacy_presence IN ('friends', 'nobody')", name=op.f("ck_users_privacy_presence")
        ),
        sa.CheckConstraint(
            "privacy_invites IN ('friends', 'nobody')", name=op.f("ck_users_privacy_invites")
        ),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_users")),
        sa.UniqueConstraint("username_lower", name=op.f("uq_users_username_lower")),
    )

    op.create_table(
        "refresh_tokens",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("user_id", sa.Uuid(), nullable=False),
        sa.Column("token_hash", sa.String(length=64), nullable=False),
        sa.Column("family_id", sa.Uuid(), nullable=False),
        _timestamp("created_at"),
        _timestamp("expires_at"),
        _timestamp("revoked_at", nullable=True),
        sa.Column("replaced_by", sa.Uuid(), nullable=True),
        _user_fk("user_id", "refresh_tokens"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_refresh_tokens")),
        sa.UniqueConstraint("token_hash", name=op.f("uq_refresh_tokens_token_hash")),
    )
    op.create_index(op.f("ix_refresh_tokens_user_id"), "refresh_tokens", ["user_id"])
    op.create_index(op.f("ix_refresh_tokens_family_id"), "refresh_tokens", ["family_id"])
    op.create_index(op.f("ix_refresh_tokens_expires_at"), "refresh_tokens", ["expires_at"])

    op.create_table(
        "friend_requests",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("from_user_id", sa.Uuid(), nullable=False),
        sa.Column("to_user_id", sa.Uuid(), nullable=False),
        _timestamp("created_at"),
        _timestamp("expires_at"),
        sa.CheckConstraint("from_user_id <> to_user_id", name=op.f("ck_friend_requests_not_self")),
        _user_fk("from_user_id", "friend_requests"),
        _user_fk("to_user_id", "friend_requests"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_friend_requests")),
        sa.UniqueConstraint(
            "from_user_id", "to_user_id", name=op.f("uq_friend_requests_from_user_id_to_user_id")
        ),
    )
    op.create_index(op.f("ix_friend_requests_to_user_id"), "friend_requests", ["to_user_id"])
    op.create_index(op.f("ix_friend_requests_expires_at"), "friend_requests", ["expires_at"])

    op.create_table(
        "friendships",
        sa.Column("user_low", sa.Uuid(), nullable=False),
        sa.Column("user_high", sa.Uuid(), nullable=False),
        _timestamp("created_at"),
        sa.CheckConstraint("user_low < user_high", name=op.f("ck_friendships_ordered_pair")),
        _user_fk("user_low", "friendships"),
        _user_fk("user_high", "friendships"),
        sa.PrimaryKeyConstraint("user_low", "user_high", name=op.f("pk_friendships")),
    )
    op.create_index(op.f("ix_friendships_user_high"), "friendships", ["user_high"])

    op.create_table(
        "blocks",
        sa.Column("blocker_id", sa.Uuid(), nullable=False),
        sa.Column("blocked_id", sa.Uuid(), nullable=False),
        _timestamp("created_at"),
        sa.CheckConstraint("blocker_id <> blocked_id", name=op.f("ck_blocks_not_self")),
        _user_fk("blocker_id", "blocks"),
        _user_fk("blocked_id", "blocks"),
        sa.PrimaryKeyConstraint("blocker_id", "blocked_id", name=op.f("pk_blocks")),
    )
    op.create_index(op.f("ix_blocks_blocked_id"), "blocks", ["blocked_id"])

    op.create_table(
        "reports",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("reporter_id", sa.Uuid(), nullable=True),
        sa.Column("reported_id", sa.Uuid(), nullable=True),
        sa.Column("reason", sa.String(length=16), nullable=False),
        sa.Column("details", sa.String(length=500), nullable=True),
        _timestamp("created_at"),
        sa.Column("status", sa.String(length=8), server_default="open", nullable=False),
        sa.CheckConstraint(
            "reason IN ('spam', 'harassment', 'cheating', 'impersonation', 'other')",
            name=op.f("ck_reports_reason"),
        ),
        sa.CheckConstraint("status IN ('open', 'closed')", name=op.f("ck_reports_status")),
        # Deleting a user keeps reports but drops the link to them.
        _user_fk("reporter_id", "reports", ondelete="SET NULL"),
        _user_fk("reported_id", "reports", ondelete="SET NULL"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_reports")),
    )
    op.create_index(op.f("ix_reports_reporter_id"), "reports", ["reporter_id"])
    op.create_index(op.f("ix_reports_reported_id"), "reports", ["reported_id"])
    op.create_index(op.f("ix_reports_created_at"), "reports", ["created_at"])

    op.create_table(
        "invites",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("from_user_id", sa.Uuid(), nullable=False),
        sa.Column("to_user_id", sa.Uuid(), nullable=False),
        sa.Column("session", sa.String(length=256), nullable=False),
        sa.Column("pack_code", sa.String(length=12), nullable=True),
        _timestamp("created_at"),
        _timestamp("expires_at"),
        sa.Column("status", sa.String(length=10), server_default="pending", nullable=False),
        sa.CheckConstraint(
            "status IN ('pending', 'accepted', 'declined')", name=op.f("ck_invites_status")
        ),
        sa.CheckConstraint("from_user_id <> to_user_id", name=op.f("ck_invites_not_self")),
        _user_fk("from_user_id", "invites"),
        _user_fk("to_user_id", "invites"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_invites")),
    )
    op.create_index(op.f("ix_invites_from_user_id"), "invites", ["from_user_id"])
    op.create_index(op.f("ix_invites_to_user_id"), "invites", ["to_user_id"])
    op.create_index(op.f("ix_invites_expires_at"), "invites", ["expires_at"])

    op.create_table(
        "packs",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("code", sa.String(length=12), nullable=False),
        sa.Column("owner_id", sa.Uuid(), nullable=True),
        sa.Column("data", sa.JSON(), nullable=False),
        _timestamp("created_at"),
        _timestamp("expires_at"),
        _user_fk("owner_id", "packs"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_packs")),
        sa.UniqueConstraint("code", name=op.f("uq_packs_code")),
    )
    op.create_index(op.f("ix_packs_owner_id"), "packs", ["owner_id"])
    op.create_index(op.f("ix_packs_expires_at"), "packs", ["expires_at"])


def downgrade() -> None:
    # Left behind on purpose by 0002's downgrade; gone with everything else here.
    op.drop_table("deleted_usernames", if_exists=True)
    for table in (
        "packs",
        "invites",
        "reports",
        "blocks",
        "friendships",
        "friend_requests",
        "refresh_tokens",
        "users",
    ):
        op.drop_table(table)
