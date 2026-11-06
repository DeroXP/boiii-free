"""Crash reports from launchers, and their stacks-only minidumps.

Revision ID: 0003
Revises: 0002
Create Date: 2026-11-05
"""

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0003"
down_revision: str | Sequence[str] | None = "0002"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamp(name: str) -> sa.Column:
    return sa.Column(name, sa.DateTime(timezone=True), nullable=False)


def upgrade() -> None:
    op.create_table(
        "crash_reports",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("report_id", sa.String(length=32), nullable=False),
        sa.Column("install_id", sa.String(length=32), nullable=False),
        _timestamp("created_utc"),
        _timestamp("received_at"),
        sa.Column("version", sa.String(length=64), nullable=False),
        sa.Column("commit", sa.String(length=64), nullable=True),
        sa.Column("dirty", sa.Boolean(), nullable=True),
        sa.Column("game_build", sa.String(length=64), nullable=True),
        sa.Column("steam_build", sa.BigInteger(), nullable=True),
        sa.Column("kind", sa.String(length=16), nullable=False),
        sa.Column("code", sa.String(length=18), nullable=True),
        sa.Column("module", sa.String(length=256), nullable=True),
        sa.Column("offset", sa.String(length=18), nullable=True),
        sa.Column("signature", sa.String(length=16), nullable=False),
        sa.Column("phase", sa.String(length=64), nullable=True),
        sa.Column("mode", sa.String(length=2), nullable=True),
        sa.Column("map", sa.String(length=256), nullable=True),
        sa.Column("plan_id", sa.String(length=256), nullable=True),
        sa.Column("safe_mode", sa.Boolean(), nullable=True),
        sa.Column("flags", sa.JSON(), nullable=True),
        sa.Column("mods", sa.JSON(), nullable=True),
        sa.Column("system", sa.JSON(), nullable=True),
        sa.Column("crash", sa.JSON(), nullable=False),
        sa.Column("logs", sa.JSON(), nullable=True),
        sa.Column("stored_bytes", sa.Integer(), nullable=False),
        sa.CheckConstraint(
            "kind IN ('exception', 'fail_fast', 'out_of_memory', 'unknown_exit')",
            name=op.f("ck_crash_reports_kind"),
        ),
        sa.PrimaryKeyConstraint("id", name=op.f("pk_crash_reports")),
        sa.UniqueConstraint("report_id", name=op.f("uq_crash_reports_report_id")),
    )
    op.create_index(op.f("ix_crash_reports_install_id"), "crash_reports", ["install_id"])
    op.create_index(op.f("ix_crash_reports_received_at"), "crash_reports", ["received_at"])
    op.create_index(op.f("ix_crash_reports_version"), "crash_reports", ["version"])
    op.create_index(op.f("ix_crash_reports_signature"), "crash_reports", ["signature"])

    op.create_table(
        "crash_minidumps",
        sa.Column("crash_id", sa.Uuid(), nullable=False),
        sa.Column("size", sa.Integer(), nullable=False),
        sa.Column("data", sa.LargeBinary(), nullable=False),
        sa.ForeignKeyConstraint(
            ["crash_id"],
            ["crash_reports.id"],
            name=op.f("fk_crash_minidumps_crash_id_crash_reports"),
            ondelete="CASCADE",
        ),
        sa.PrimaryKeyConstraint("crash_id", name=op.f("pk_crash_minidumps")),
    )


def downgrade() -> None:
    # Revision 0002 has no crash reports, so they go with their tables (and indexes).
    op.drop_table("crash_minidumps")
    op.drop_table("crash_reports")
