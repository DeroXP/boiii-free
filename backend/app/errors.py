"""One error shape for every failure: {"error": {"code": ..., "message": ...}}."""

from http import HTTPStatus

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

# Sent with every response by app.security.SecurityHeadersMiddleware, and by the 500 handler below.
SECURITY_HEADERS = {
    "X-Content-Type-Options": "nosniff",
    "Cache-Control": "no-store",
    "Referrer-Policy": "no-referrer",
}
API_CONTENT_SECURITY_POLICY = "default-src 'none'; frame-ancestors 'none'"

_CODES = {
    400: "bad_request",
    401: "unauthorized",
    403: "forbidden",
    404: "not_found",
    405: "method_not_allowed",
    409: "conflict",
    413: "payload_too_large",
    415: "unsupported_media_type",
    422: "invalid_request",
    429: "rate_limited",
    500: "internal_error",
    503: "unavailable",
}


class ApiError(Exception):
    """An expected failure that maps straight to an HTTP response."""

    def __init__(
        self,
        status_code: int,
        code: str,
        message: str,
        headers: dict[str, str] | None = None,
    ) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.code = code
        self.message = message
        self.headers = headers


def not_found(message: str = "Not found.") -> ApiError:
    return ApiError(404, "not_found", message)


def error_response(
    status_code: int,
    code: str,
    message: str,
    headers: dict[str, str] | None = None,
) -> JSONResponse:
    return JSONResponse(
        {"error": {"code": code, "message": message}},
        status_code=status_code,
        headers=headers,
    )


def _validation_message(exc: RequestValidationError) -> str:
    # Pydantic errors carry the offending input; only the location and the rule are echoed back,
    # so a rejected password never comes back in a response.
    errors = exc.errors()
    if not errors:
        return "The request is not valid."
    first = errors[0]
    location = ".".join(str(part) for part in first.get("loc", ()) if part != "body")
    message = str(first.get("msg", "Invalid value.")).removeprefix("Value error, ")
    return f"{location}: {message}" if location else message


async def _api_error(_request: Request, exc: Exception) -> JSONResponse:
    assert isinstance(exc, ApiError)
    return error_response(exc.status_code, exc.code, exc.message, exc.headers)


async def _http_error(_request: Request, exc: Exception) -> JSONResponse:
    assert isinstance(exc, StarletteHTTPException)
    code = _CODES.get(exc.status_code, "error")
    if isinstance(exc.detail, str) and exc.detail:
        message = exc.detail
    else:
        message = HTTPStatus(exc.status_code).phrase
    return error_response(exc.status_code, code, message, getattr(exc, "headers", None))


async def _validation_error(_request: Request, exc: Exception) -> JSONResponse:
    assert isinstance(exc, RequestValidationError)
    return error_response(422, "invalid_request", _validation_message(exc))


async def _unhandled_error(_request: Request, _exc: Exception) -> JSONResponse:
    # Starlette re-raises after this response, so the server still logs the traceback. The
    # response leaves through ServerErrorMiddleware, outside SecurityHeadersMiddleware, so it
    # carries the security headers itself.
    return error_response(
        500,
        "internal_error",
        "Something went wrong on our side.",
        headers={**SECURITY_HEADERS, "Content-Security-Policy": API_CONTENT_SECURITY_POLICY},
    )


def install_error_handlers(app: FastAPI) -> None:
    app.add_exception_handler(ApiError, _api_error)
    app.add_exception_handler(StarletteHTTPException, _http_error)
    app.add_exception_handler(RequestValidationError, _validation_error)
    app.add_exception_handler(Exception, _unhandled_error)
