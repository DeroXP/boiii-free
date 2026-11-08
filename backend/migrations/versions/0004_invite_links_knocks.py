"""Invite links and knocks: a host's link that asks to join, and the players who followed it.

Revision ID: 0004
Revises: 0003
Create Date: 2026-11-07
"""

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0004"
down_revision: str | Sequence[str] | None = "0003"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamp(name: str) -> sa.Column:
    return sa.Column(name, sa.DateTime(timezone=True), nullable=False)


def _user_fk(column: str, table: str) -> sa.ForeignKeyConstraint:
    return sa.ForeignKeyConstraint(
        [column], ["users.id"], name=op.f(f"fk_{table}_{column}_users"), ondelete="CASCADE"
    )


def upgrade() -> None:
    op.create_table(
        "invite_links",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("code", sa.String(length=12), nullable=False),
        sa.Column("owner_id", sa.Uuid(), nullable=False),
        sa.Column("mode", sa.String(length=2), nullable=False),
        _timestamp("created_at"),
        _timestamp("expires_at"),
        sa.CheckConstraint("mode IN ('zm', 'mp', 'cp')", name=op.f("ck_invite_links_mode")),
        _user_fk("owner_id", "invite_links"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_invite_links")),
        sa.UniqueConstraint("code", name=op.f("uq_invite_links_code")),
    )
    op.create_index(op.f("ix_invite_links_owner_id"), "invite_links", ["owner_id"])
    op.create_index(op.f("ix_invite_links_expires_at"), "invite_links", ["expires_at"])

    op.create_table(
        "knocks",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("from_user_id", sa.Uuid(), nullable=False),
        sa.Column("to_user_id", sa.Uuid(), nullable=False),
        sa.Column("mode", sa.String(length=2), nullable=False),
        _timestamp("created_at"),
        _timestamp("expires_at"),
        sa.Column("status", sa.String(length=10), server_default="pending", nullable=False),
        sa.CheckConstraint(
            "status IN ('pending', 'accepted', 'declined')", name=op.f("ck_knocks_status")
        ),
        sa.CheckConstraint("mode IN ('zm', 'mp', 'cp')", name=op.f("ck_knocks_mode")),
        sa.CheckConstraint("from_user_id <> to_user_id", name=op.f("ck_knocks_not_self")),
        _user_fk("from_user_id", "knocks"),
        _user_fk("to_user_id", "knocks"),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_knocks")),
    )
    op.create_index(op.f("ix_knocks_from_user_id"), "knocks", ["from_user_id"])
    op.create_index(op.f("ix_knocks_to_user_id"), "knocks", ["to_user_id"])
    op.create_index(op.f("ix_knocks_expires_at"), "knocks", ["expires_at"])


def downgrade() -> None:
    op.drop_table("knocks")
    op.drop_table("invite_links")
