"""Username, password and email rules."""

import httpx
import pytest

from app.schemas import normalize_username, username_lookup_key


async def _register(client: httpx.AsyncClient, **body: str) -> httpx.Response:
    body.setdefault("password", "a fine long password")
    return await client.post("/v1/auth/register", json=body)


@pytest.mark.parametrize(
    "username",
    [
        "ab",  # too short
        "a" * 25,  # too long
        "has space",
        "émile",  # not ASCII after NFKC
        "semi;colon",
        "tab\tname",
        "new\nline",
        "...",  # no letter or digit
        "admin",
        "Administrator",
        "xerces",
        "XERCES",
        "x.e.r.c.e.s",
        "support",
        "system",
        "moderator",
        "root",
        "staff",
        "official",
    ],
)
async def test_bad_usernames(client: httpx.AsyncClient, username: str) -> None:
    response = await _register(client, username=username)
    assert response.status_code == 422, username
    error = response.json()["error"]
    assert error["code"] == "invalid_request"
    assert error["message"].startswith("username: ")


@pytest.mark.parametrize("username", ["abc", "a" * 24, "Name_with.dots-1", "x_X", "007"])
async def test_good_usernames(client: httpx.AsyncClient, username: str) -> None:
    response = await _register(client, username=username)
    assert response.status_code == 201, response.text
    assert response.json()["user"]["username"] == username


async def test_username_nfkc_normalization(client: httpx.AsyncClient) -> None:
    # Full-width letters normalize to ASCII, so they collide with the plain name.
    response = await _register(client, username="ｔｅｓｔer")
    assert response.status_code == 201
    assert response.json()["user"]["username"] == "tester"
    clash = await _register(client, username="TESTER")
    assert clash.status_code == 409
    assert normalize_username("ＡＢＣ") == "ABC"
    assert username_lookup_key("ＡＢＣ") == "abc"
    assert username_lookup_key("admin") is None


@pytest.mark.parametrize(
    ("username", "password", "reason"),
    [
        ("pwuser1", "short", "too short"),
        ("pwuser2", "x" * 129, "too long"),
        ("LongUsername1", "longusername1", "same as username"),
        ("LongUsername2", "LONGUSERNAME2", "same as username, any case"),
    ],
)
async def test_bad_passwords(
    client: httpx.AsyncClient, username: str, password: str, reason: str
) -> None:
    response = await _register(client, username=username, password=password)
    assert response.status_code == 422, reason
    # The rejected password is never echoed back.
    assert password not in response.text


async def test_password_boundaries(client: httpx.AsyncClient) -> None:
    assert (await _register(client, username="ten", password="x" * 10)).status_code == 201
    assert (await _register(client, username="max", password="y" * 128)).status_code == 201


@pytest.mark.parametrize(
    "email",
    ["not-an-email", "a@b", "@example.com", "a@.com", "a@example.", "a b@example.com", "a@ex..com"],
)
async def test_bad_emails(client: httpx.AsyncClient, email: str) -> None:
    response = await _register(client, username="mailer", email=email)
    assert response.status_code == 422


async def test_email_optional_and_lowercased(client: httpx.AsyncClient) -> None:
    none = await _register(client, username="nomail")
    assert none.json()["user"]["email"] is None
    blank = await _register(client, username="blankmail", email="  ")
    assert blank.json()["user"]["email"] is None
    mixed = await _register(client, username="mixedmail", email="Mixed.Case@Example.ORG")
    assert mixed.json()["user"]["email"] == "mixed.case@example.org"


async def test_unknown_fields_rejected(client: httpx.AsyncClient) -> None:
    response = await _register(client, username="extra", is_admin="true")
    assert response.status_code == 422


async def test_malformed_json(client: httpx.AsyncClient) -> None:
    response = await client.post(
        "/v1/auth/login", content=b"{not json", headers={"Content-Type": "application/json"}
    )
    assert response.status_code == 422
    assert response.json()["error"]["code"] == "invalid_request"
