"""Short share codes for packs: XRC- plus 8 Crockford base32 characters, kept 90 days.

A pack references mods; it never contains them. Validation keeps it small and keeps local paths
(which leak user names) out of anything other people can fetch.
"""

import json
import math
import re
import secrets
import uuid
from datetime import timedelta
from typing import Any

from fastapi import APIRouter, Path, Request, Response
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession

from app.deps import DbDep, ServicesDep, UserDep
from app.errors import ApiError, not_found
from app.log import log_event
from app.models import Pack, utcnow
from app.schemas import PACK_CODE_ALPHABET, PackCreatedOut, PackIn, PackOut, normalize_pack_code

router = APIRouter(prefix="/v1/packs", tags=["packs"])

PACK_TTL = timedelta(days=90)
MAX_PACK_BYTES = 64 * 1024
MAX_PACK_NAME = 64
MAX_PACK_MODS = 128
MAX_PACK_DEPTH = 32
CODE_ATTEMPTS = 5

MOD_ID = re.compile(r"[a-z0-9][a-z0-9._+-]{1,127}")
# file:///... and smb://server/share (a network share, like \\server\share), anywhere.
_FILE_URL = re.compile(r"(?<![A-Za-z0-9+.-])(?:file|smb):[\\/]", re.IGNORECASE)
# Other URLs (https://, steam://) name places on someone's server, not on this machine. The
# scheme's length is capped so that a long run of letters cannot make this quadratic.
_URL = re.compile(r"[A-Za-z][A-Za-z0-9+.-]{0,31}://[^\s\"'<>]*")
_LOCAL_PATH_PATTERNS = (
    re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/]"),  # C:\... or D:/...
    re.compile(r"\\\\[^\\/\s]+[\\/]"),  # \\server\share\..., anywhere
    re.compile(r"^\s*\\"),  # \rooted\path
    re.compile(r"(?<![\w.~+-])~[\\/]"),  # ~/...
    re.compile(r"%(?:USERPROFILE|APPDATA|LOCALAPPDATA|HOMEPATH|TEMP|TMP)%", re.IGNORECASE),
)
# An absolute path: a slash or backslash that starts a word, up to the next space or quote.
_ABSOLUTE_PATH = re.compile(r"(?<![\w.~+-])[\\/][^\s\"'<>|]*")
# A folder in one that holds user names or local drives, in any letter case: /home/<name>,
# /var/home/<name>, /Users/<name>, /root/, /media/<name>, /run/media/<name>, /mnt/c/ and
# /cygdrive/c/.
_HOME_FOLDER = re.compile(
    r"[\\/](?:home|users|root|media|cygdrive|mnt[\\/][a-z])(?:[\\/]|$)", re.IGNORECASE
)


class PackError(ValueError):
    pass


def looks_like_local_path(value: str) -> bool:
    if _FILE_URL.search(value):
        return True
    text = _URL.sub(" ", value)
    if any(pattern.search(text) for pattern in _LOCAL_PATH_PATTERNS):
        return True
    # Two simple passes instead of one regex with nested repeats, which a 64 KB string of slashes
    # could make take quadratic time.
    return any(_HOME_FOLDER.search(path.group()) for path in _ABSOLUTE_PATH.finditer(text))


def _check_string(value: str) -> None:
    if "\x00" in value:
        raise PackError("pack contains a NUL character.")
    if looks_like_local_path(value):
        raise PackError("pack contains what looks like a local file path; packs must not.")


def validate_pack(pack: Any) -> dict[str, Any]:
    if not isinstance(pack, dict):
        raise PackError("pack must be a JSON object.")
    try:
        encoded = json.dumps(pack, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    except ValueError:
        raise PackError("pack must not contain NaN or Infinity.") from None
    try:
        # A lone surrogate (JSON "\ud800") gets through JSON parsing but is not valid Unicode.
        size = len(encoded.encode("utf-8"))
    except UnicodeEncodeError:
        raise PackError("pack contains text that is not valid Unicode.") from None
    if size > MAX_PACK_BYTES:
        raise PackError(f"pack is larger than {MAX_PACK_BYTES // 1024} KB.")

    schema = pack.get("schema")
    if type(schema) is not int or schema != 1:  # True == 1 in Python, so compare the type too
        raise PackError("pack.schema must be 1.")
    name = pack.get("name")
    if not isinstance(name, str) or not name.strip() or len(name) > MAX_PACK_NAME:
        raise PackError(f"pack.name must be 1 to {MAX_PACK_NAME} characters.")
    mods = pack.get("mods")
    if not isinstance(mods, list) or len(mods) > MAX_PACK_MODS:
        raise PackError(f"pack.mods must be a list of at most {MAX_PACK_MODS} mods.")
    for index, mod in enumerate(mods):
        mod_id = mod.get("id") if isinstance(mod, dict) else None
        if not isinstance(mod_id, str) or not MOD_ID.fullmatch(mod_id):
            raise PackError(
                f"pack.mods[{index}].id must match {MOD_ID.pattern} (lower case, 2-128 chars)."
            )

    # Walk every key and value without recursion (deep nesting cannot blow the stack).
    stack: list[tuple[Any, int]] = [(pack, 0)]
    while stack:
        node, depth = stack.pop()
        if depth > MAX_PACK_DEPTH:
            raise PackError("pack is nested too deeply.")
        if isinstance(node, dict):
            for key, value in node.items():
                _check_string(key)
                stack.append((value, depth + 1))
        elif isinstance(node, list):
            stack.extend((item, depth + 1) for item in node)
        elif isinstance(node, str):
            _check_string(node)
        elif isinstance(node, float) and not math.isfinite(node):
            raise PackError("pack must not contain NaN or Infinity.")
    return pack


def new_pack_code() -> str:
    return "XRC-" + "".join(secrets.choice(PACK_CODE_ALPHABET) for _ in range(8))


@router.post("", status_code=201, response_model=PackCreatedOut)
async def create_pack(
    body: PackIn, user: UserDep, db: DbDep, services: ServicesDep
) -> PackCreatedOut:
    # Charged before validating, so a stream of rejected packs costs the sender too.
    services.limiter.enforce("pack_create", str(user.id))
    try:
        pack = validate_pack(body.pack)
    except PackError as exc:
        raise ApiError(422, "invalid_pack", str(exc)) from None
    now = utcnow()
    for _ in range(CODE_ATTEMPTS):
        code = new_pack_code()
        if await db.scalar(select(Pack.id).where(Pack.code == code)) is not None:
            continue
        row = Pack(
            id=uuid.uuid4(),
            code=code,
            owner_id=user.id,
            data=pack,
            created_at=now,
            expires_at=now + PACK_TTL,
        )
        db.add(row)
        try:
            await db.commit()
        except IntegrityError:  # someone took the same code in between; try another
            await db.rollback()
            continue
        log_event("pack_created", pack_id=str(row.id), user_id=str(user.id))
        return PackCreatedOut(code=code, expires_at=row.expires_at)
    raise ApiError(503, "unavailable", "Could not allocate a share code. Try again.")


async def _live_pack(db: AsyncSession, code: str) -> Pack:
    normalized = normalize_pack_code(code)
    if normalized is None:
        raise not_found("Pack not found.")
    row = await db.scalar(select(Pack).where(Pack.code == normalized))
    if row is None or row.expires_at <= utcnow():
        raise not_found("Pack not found.")
    return row


@router.get("/{code}", response_model=PackOut)
async def get_pack(
    request: Request,
    db: DbDep,
    services: ServicesDep,
    code: str = Path(max_length=32),
) -> PackOut:
    services.limiter.enforce("pack_fetch", services.address_key(request.scope))
    row = await _live_pack(db, code)
    return PackOut(code=row.code, pack=row.data, created_at=row.created_at, expires_at=row.expires_at)


@router.delete("/{code}", status_code=204)
async def delete_pack(
    user: UserDep, db: DbDep, code: str = Path(max_length=32)
) -> Response:
    row = await _live_pack(db, code)
    if row.owner_id != user.id:
        raise ApiError(403, "forbidden", "Only the owner can delete this pack.")
    await db.delete(row)
    await db.commit()
    return Response(status_code=204)
