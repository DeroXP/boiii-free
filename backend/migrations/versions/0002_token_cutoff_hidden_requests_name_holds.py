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

    op.create_table(
        "deleted_usernames",
        sa.Column("username_lower", sa.String(length=24), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.PrimaryKeyConstraint("username_lower", name=op.f("pk_deleted_usernames")),
    )
    op.create_index(
        op.f("ix_deleted_usernames_expires_at"), "deleted_usernames", ["expires_at"]
    )


def downgrade() -> None:
    op.drop_index(op.f("ix_deleted_usernames_expires_at"), table_name="deleted_usernames")
    op.drop_table("deleted_usernames")
    with op.batch_alter_table("friend_requests") as batch:
        batch.drop_column("hidden")
    with op.batch_alter_table("users") as batch:
        batch.drop_column("tokens_valid_after")
