"""GET /v1/me/address: the caller's own public address, for a game invite (DESIGN.md 10.3)."""

from pathlib import Path

import httpx
import pytest

from tests.helpers import AppFactory, client_for, make_settings, register, strict_rules


async def _address(client: httpx.AsyncClient, headers: dict[str, str], forwarded: str):
    return await client.get(
        "/v1/me/address", headers={**headers, "X-Forwarded-For": forwarded}
    )


async def test_the_public_address_comes_back_exactly(client: httpx.AsyncClient) -> None:
    alice = await register(client, "alice")
    v4 = await _address(client, alice.headers, "8.8.8.8")
    assert v4.status_code == 200, v4.text
    assert v4.json() == {"address": "8.8.8.8"}
    # IPv6 as it is, not the /64 that rate limits use.
    v6 = await _address(client, alice.headers, "2001:4860:4860::8888")
    assert v6.json() == {"address": "2001:4860:4860::8888"}
    mapped = await _address(client, alice.headers, "::ffff:8.8.4.4")
    assert mapped.json() == {"address": "8.8.4.4"}
    with_port = await _address(client, alice.headers, "[2001:4860:4860::8844]:443")
    assert with_port.json() == {"address": "2001:4860:4860::8844"}


@pytest.mark.parametrize(
    "forwarded",
    ["10.1.2.3", "192.168.1.251", "127.0.0.1", "198.51.100.7", "::1", "fe80::1", "nonsense"],
)
async def test_an_address_that_is_not_public_is_not_handed_out(
    client: httpx.AsyncClient, forwarded: str
) -> None:
    alice = await register(client, "alice")
    response = await _address(client, alice.headers, forwarded)
    assert response.status_code == 503
    assert response.json()["error"]["code"] == "address_unknown"


async def test_only_signed_in_callers_get_it(client: httpx.AsyncClient) -> None:
    response = await client.get("/v1/me/address", headers={"X-Forwarded-For": "8.8.8.8"})
    assert response.status_code == 401


async def test_the_proxy_settings_pick_the_entry(app_factory: AppFactory, tmp_path: Path) -> None:
    app = await app_factory(settings=make_settings(tmp_path, XERCES_PROXY_HOPS=2))
    async with client_for(app) as client:
        alice = await register(client, "alice")
        # Two appending proxies: the second entry from the right is what the outer one saw; the
        # leftmost came from the client and proves nothing.
        response = await _address(client, alice.headers, "9.9.9.9, 1.1.1.1, 10.0.0.5")
        assert response.json() == {"address": "1.1.1.1"}


async def test_without_a_trusted_proxy_the_socket_address_counts(
    app_factory: AppFactory, tmp_path: Path
) -> None:
    app = await app_factory(settings=make_settings(tmp_path, XERCES_TRUST_PROXY=False))
    async with client_for(app) as client:
        alice = await register(client, "alice")
        # The test client's own address is not public, and the forged header is ignored.
        response = await _address(client, alice.headers, "8.8.8.8")
        assert response.status_code == 503


async def test_it_is_rate_limited_per_account(app_factory: AppFactory) -> None:
    app = await app_factory(rules=strict_rules("own_address"))
    async with client_for(app) as client:
        alice = await register(client, "alice")
        codes = [(await _address(client, alice.headers, "8.8.8.8")).status_code for _ in range(31)]
        assert codes[:30] == [200] * 30
        assert codes[30] == 429
