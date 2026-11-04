"""Print fresh secrets for the Xerces backend, ready to paste into Railway's variable editor.

    python scripts/genkey.py                 # three KEY=value lines on stdout
    python scripts/genkey.py --public SEED   # PEM public key for XERCES_TOKEN_EXTRA_PUBLIC_KEYS

The signing key is a random 32-byte Ed25519 seed, so generating one needs only the standard
library. Deriving its public key (only needed when rotating keys) needs `cryptography`.
"""

import argparse
import base64
import datetime
import secrets
import sys


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def public_pem(seed: str) -> str:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    raw = base64.urlsafe_b64decode(seed + "=" * (-len(seed) % 4))
    key = Ed25519PrivateKey.from_private_bytes(raw)
    return key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    ).decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--public", metavar="SEED", help="print the public key of this seed")
    args = parser.parse_args()
    if args.public:
        print(public_pem(args.public), end="")
        return 0

    key_id = f"{datetime.date.today():%Y-%m-%d}-{secrets.token_hex(2)}"
    print(f"XERCES_TOKEN_PRIVATE_KEY={b64url(secrets.token_bytes(32))}")
    print(f"XERCES_TOKEN_KEY_ID={key_id}")
    print(f"XERCES_RATE_LIMIT_SECRET={b64url(secrets.token_bytes(32))}")
    print(
        "# Paste these into the service's variables. Do not commit them anywhere.",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
