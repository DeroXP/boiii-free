"""Request and response bodies, and the validators they share.

Request models forbid unknown fields and cap every string, so nothing unbounded reaches the
database. Validation messages never include the submitted value.
"""

import re
import unicodedata
import uuid
from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictBool, field_validator, model_validator

from app.models import User
from app.passwords import normalize_password

USERNAME_MIN = 3
USERNAME_MAX = 24
PASSWORD_MIN = 10
PASSWORD_MAX = 128
RESERVED_USERNAMES = frozenset(
    {
        "admin",
        "administrator",
        "xerces",
        "support",
        "system",
        "moderator",
        "root",
        "staff",
        "official",
    }
)
PACK_CODE_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  # Crockford base32

_USERNAME = re.compile(r"[A-Za-z0-9_.-]+")
_USERNAME_PUNCTUATION = str.maketrans("", "", "_.-")
_CONTROL = re.compile(r"[\x00-\x1f\x7f]")
_CONTROL_EXCEPT_WHITESPACE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
_EMAIL = re.compile(r"[^@\s]{1,64}@[^@\s]{1,253}")
_PACK_CODE = re.compile(r"XRC-[0-9A-HJKMNP-TV-Z]{8}")
_CROCKFORD_LOOKALIKES = str.maketrans({"O": "0", "I": "1", "L": "1"})


def normalize_username(value: str) -> str:
    """Return the NFKC form of a new username, or raise ValueError with a readable reason."""
    name = unicodedata.normalize("NFKC", value)
    if not USERNAME_MIN <= len(name) <= USERNAME_MAX:
        raise ValueError(f"Username must be {USERNAME_MIN} to {USERNAME_MAX} characters.")
    if not _USERNAME.fullmatch(name):
        raise ValueError("Username may only use letters A-Z, digits, '_', '.' and '-'.")
    if not any(ch.isalnum() for ch in name):
        raise ValueError("Username must contain a letter or a digit.")
    folded = name.lower()
    if folded in RESERVED_USERNAMES or folded.translate(_USERNAME_PUNCTUATION) in RESERVED_USERNAMES:
        raise ValueError("That username is reserved.")
    return name


def username_lookup_key(value: str) -> str | None:
    """Lower-case key for a typed username, or None when no account can have that name."""
    try:
        return normalize_username(value).lower()
    except ValueError:
        return None


def check_new_password(value: str) -> str:
    password = normalize_password(value)
    if not PASSWORD_MIN <= len(password) <= PASSWORD_MAX:
        raise ValueError(f"Password must be {PASSWORD_MIN} to {PASSWORD_MAX} characters.")
    return password


def password_matches_username(password: str, username: str) -> bool:
    return normalize_password(password).casefold() == username.casefold()


def normalize_email(value: str | None) -> str | None:
    if value is None:
        return None
    email = value.strip()
    if not email:
        return None
    if len(email) > 254 or _CONTROL.search(email) or not _EMAIL.fullmatch(email):
        raise ValueError("Email address is not valid.")
    domain = email.rsplit("@", 1)[1]
    if "." not in domain or domain.startswith(".") or domain.endswith(".") or ".." in domain:
        raise ValueError("Email address is not valid.")
    return email.lower()


def normalize_pack_code(value: str) -> str | None:
    """Canonical XRC-XXXXXXXX code, forgiving case and Crockford look-alikes (O, I, L)."""
    text = value.strip().upper()
    if text.startswith("XRC-"):
        body = text[4:]
    elif text.startswith("XRC"):
        body = text[3:]
    else:
        return None
    code = "XRC-" + body.replace("-", "").translate(_CROCKFORD_LOOKALIKES)
    return code if _PACK_CODE.fullmatch(code) else None


class _Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


# ----------------------------------------------------------------------------- accounts


class RegisterIn(_Input):
    username: str = Field(max_length=64)
    password: str = Field(max_length=1024)
    email: str | None = Field(default=None, max_length=320)

    @field_validator("username")
    @classmethod
    def _username(cls, value: str) -> str:
        return normalize_username(value)

    @field_validator("password")
    @classmethod
    def _password(cls, value: str) -> str:
        return check_new_password(value)

    @field_validator("email")
    @classmethod
    def _email(cls, value: str | None) -> str | None:
        return normalize_email(value)

    @model_validator(mode="after")
    def _password_is_not_username(self) -> "RegisterIn":
        if password_matches_username(self.password, self.username):
            raise ValueError("Password must not be the same as the username.")
        return self


class LoginIn(_Input):
    username: str = Field(min_length=1, max_length=64)
    password: str = Field(min_length=1, max_length=1024)


class RefreshIn(_Input):
    refresh_token: str = Field(min_length=1, max_length=256)


class PasswordChangeIn(_Input):
    current_password: str = Field(min_length=1, max_length=1024)
    new_password: str = Field(max_length=1024)

    @field_validator("new_password")
    @classmethod
    def _new_password(cls, value: str) -> str:
        return check_new_password(value)


class PasswordConfirmIn(_Input):
    password: str = Field(min_length=1, max_length=1024)


class PrivacyPatch(_Input):
    friend_requests: Literal["everyone", "nobody"] | None = None
    presence: Literal["friends", "nobody"] | None = None
    invites: Literal["friends", "nobody"] | None = None


class PrivacyOut(BaseModel):
    friend_requests: Literal["everyone", "nobody"]
    presence: Literal["friends", "nobody"]
    invites: Literal["friends", "nobody"]


class MeOut(BaseModel):
    """The signed-in user's own account. Never returned for anyone else (email is private)."""

    id: uuid.UUID
    username: str
    email: str | None
    privacy: PrivacyOut
    created_at: datetime

    @classmethod
    def of(cls, user: User) -> "MeOut":
        return cls(
            id=user.id,
            username=user.username,
            email=user.email,
            privacy=PrivacyOut(
                friend_requests=user.privacy_friend_requests,  # type: ignore[arg-type]
                presence=user.privacy_presence,  # type: ignore[arg-type]
                invites=user.privacy_invites,  # type: ignore[arg-type]
            ),
            created_at=user.created_at,
        )


class TokenOut(BaseModel):
    access_token: str
    refresh_token: str
    token_type: Literal["Bearer"] = "Bearer"
    expires_in: int


class AuthOut(BaseModel):
    user: MeOut
    access_token: str
    refresh_token: str
    token_type: Literal["Bearer"] = "Bearer"
    expires_in: int


# ----------------------------------------------------------------------------- presence


class Activity(BaseModel):
    model_config = ConfigDict(extra="forbid")

    mode: Literal["zm", "mp", "cp"] | None = None
    joinable: StrictBool = False
    pack_code: str | None = Field(default=None, max_length=32)

    @field_validator("pack_code")
    @classmethod
    def _pack_code(cls, value: str | None) -> str | None:
        if value is None:
            return None
        code = normalize_pack_code(value)
        if code is None:
            raise ValueError("pack_code must look like XRC-XXXXXXXX.")
        return code


class PresenceIn(BaseModel):
    """{"type": "presence", ...} sent by the client over /v1/ws."""

    model_config = ConfigDict(extra="forbid")

    type: Literal["presence"]
    status: Literal["online", "away", "in_game"]
    activity: Activity | None = None


class PresenceOut(BaseModel):
    status: Literal["online", "away", "in_game"]
    activity: Activity | None = None


# ----------------------------------------------------------------------------- friends


class UsernameIn(_Input):
    username: str = Field(min_length=1, max_length=64)


class StatusOut(BaseModel):
    status: str


class FriendOut(BaseModel):
    user_id: uuid.UUID
    username: str
    since: datetime
    presence: PresenceOut | None = None


class FriendRequestOut(BaseModel):
    id: uuid.UUID
    user_id: uuid.UUID
    username: str
    created_at: datetime
    expires_at: datetime


class FriendRequestsOut(BaseModel):
    incoming: list[FriendRequestOut]
    outgoing: list[FriendRequestOut]


class BlockOut(BaseModel):
    user_id: uuid.UUID
    username: str
    created_at: datetime


class ReportIn(_Input):
    username: str = Field(min_length=1, max_length=64)
    reason: Literal["spam", "harassment", "cheating", "impersonation", "other"]
    details: str | None = Field(default=None, max_length=500)

    @field_validator("details")
    @classmethod
    def _details(cls, value: str | None) -> str | None:
        if value is None:
            return None
        if _CONTROL_EXCEPT_WHITESPACE.search(value):
            raise ValueError("Details must not contain control characters.")
        return value.strip() or None


# ----------------------------------------------------------------------------- invites


class InviteIn(_Input):
    to_user_id: uuid.UUID
    session: str = Field(min_length=1, max_length=256)
    pack_code: str | None = Field(default=None, max_length=32)

    @field_validator("session")
    @classmethod
    def _session(cls, value: str) -> str:
        if _CONTROL.search(value):
            raise ValueError("Session must not contain control characters.")
        return value

    @field_validator("pack_code")
    @classmethod
    def _pack_code(cls, value: str | None) -> str | None:
        if value is None:
            return None
        code = normalize_pack_code(value)
        if code is None:
            raise ValueError("pack_code must look like XRC-XXXXXXXX.")
        return code


class InviteOut(BaseModel):
    id: uuid.UUID
    from_user_id: uuid.UUID
    from_username: str
    to_user_id: uuid.UUID
    to_username: str
    session: str
    pack_code: str | None
    status: Literal["pending", "accepted", "declined"]
    created_at: datetime
    expires_at: datetime


class InvitesOut(BaseModel):
    incoming: list[InviteOut]
    outgoing: list[InviteOut]


# ----------------------------------------------------------------------------- packs


class PackIn(_Input):
    pack: dict[str, Any]


class PackCreatedOut(BaseModel):
    code: str
    expires_at: datetime


class PackOut(BaseModel):
    code: str
    pack: dict[str, Any]
    created_at: datetime
    expires_at: datetime
