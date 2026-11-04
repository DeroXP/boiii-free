"""Access-token cut-off per user, sender-only friend requests, and held names of deleted accounts.

Revision ID: 0002
Revises: 0001
Create Date: 2026-11-04
"""

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0002"
down_revision: str | Sequence[str] | None = "0001"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    # Batch mode is plain ALTER TABLE on PostgreSQL; SQLite needs it to drop columns again.
    with op.batch_alter_table("users") as batch:
        batch.add_column(sa.Column("tokens_valid_after", sa.DateTime(timezone=True), nullable=True))
    with op.batch_alter_table("friend_requests") as batch:
        batch.add_column(
            sa.Column("hidden", sa.Boolean(), server_default=sa.false(), nullable=False)
        )

    # A downgrade keeps this table (see below), so it may be here already, names and all.
    op.create_table(
        "deleted_usernames",
        sa.Column("username_lower", sa.String(length=24), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.PrimaryKeyConstraint("username_lower", name=op.f("pk_deleted_usernames")),
        if_not_exists=True,
    )
    op.create_index(
        op.f("ix_deleted_usernames_expires_at"),
        "deleted_usernames",
        ["expires_at"],
        if_not_exists=True,
    )


def downgrade() -> None:
    # Revision 0001 has no way to keep a request from its recipient, so a hidden one would turn
    # into an ordinary request, shown to someone who blocked the sender or accepts none. They go
    # instead, which to their senders looks like a decline.
    op.execute(sa.text("DELETE FROM friend_requests WHERE hidden"))
    with op.batch_alter_table("friend_requests") as batch:
        batch.drop_column("hidden")
    with op.batch_alter_table("users") as batch:
        batch.drop_column("tokens_valid_after")
    # deleted_usernames stays: code at 0001 never reads it, and dropping it would free every held
    # name for good. Upgrading again keeps using it (0001's downgrade drops it).
