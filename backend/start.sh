#!/bin/sh
# Container entry point: bring the database schema up to date, then serve the API.
set -eu

alembic upgrade head

# Railway's edge proxy is the only way into this container, so with XERCES_TRUST_PROXY=1 (set it
# on Railway) uvicorn trusts its X-Forwarded-* headers (--forwarded-allow-ips="*"). The app reads
# X-Forwarded-For itself for rate limits (XERCES_TRUST_PROXY, XERCES_PROXY_HOPS). Without that
# variable uvicorn ignores the headers too; otherwise it would put the first X-Forwarded-For entry,
# which any client can write, in place of the real peer address. The patterns match the values
# the app reads as true (any case).
case "${XERCES_TRUST_PROXY:-}" in
  1 | [Tt] | [Tt][Rr][Uu][Ee] | [Yy] | [Yy][Ee][Ss] | [Oo][Nn])
    set -- --proxy-headers --forwarded-allow-ips="*"
    ;;
  *)
    set -- --no-proxy-headers
    ;;
esac

# uvicorn's access log is off because it records client addresses; the app writes its own JSON
# request log without them.
exec uvicorn app.main:app \
  --host 0.0.0.0 \
  --port "${PORT:-8080}" \
  "$@" \
  --no-access-log \
  --no-server-header \
  --ws-max-size 16384 \
  --timeout-graceful-shutdown 10
