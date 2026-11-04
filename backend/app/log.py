"""JSON-lines logging to stdout.

Log event names and ids only: never tokens, passwords, emails or client addresses. uvicorn's
access log is switched off and its per-connection lines (which start with the client address)
are filtered out.
"""

import copy
import json
import logging
import logging.config
from datetime import UTC, datetime
from typing import Any

LOGGER_NAME = "xerces_backend"
logger = logging.getLogger(LOGGER_NAME)

_BASE_KEYS = frozenset({"ts", "level", "logger", "event", "message"})


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, Any] = {
            "ts": datetime.fromtimestamp(record.created, UTC)
            .isoformat(timespec="milliseconds")
            .replace("+00:00", "Z"),
            "level": record.levelname.lower(),
            "logger": record.name,
        }
        if record.name == LOGGER_NAME or record.name.startswith(LOGGER_NAME + "."):
            payload["event"] = record.getMessage()
        else:
            payload["event"] = "log"
            payload["message"] = record.getMessage()
        fields = getattr(record, "fields", None)
        if isinstance(fields, dict):
            for key, value in fields.items():
                if key not in _BASE_KEYS:
                    payload[key] = value
        if record.exc_info and record.exc_info[0] is not None:
            payload["exc_type"] = record.exc_info[0].__name__
            payload["traceback"] = self.formatException(record.exc_info)
        return json.dumps(payload, default=str, ensure_ascii=False, separators=(",", ":"))


class NoClientAddressFilter(logging.Filter):
    """Drop uvicorn lines formatted as '<client address> - "...' (WebSocket accept/reject)."""

    def filter(self, record: logging.LogRecord) -> bool:
        message = record.msg
        return not (isinstance(message, str) and message.startswith('%s - "'))


LOGGING_CONFIG: dict[str, Any] = {
    "version": 1,
    "disable_existing_loggers": False,
    "filters": {"no_client_address": {"()": NoClientAddressFilter}},
    "formatters": {"json": {"()": JsonFormatter}},
    "handlers": {
        "stdout": {
            "class": "logging.StreamHandler",
            "stream": "ext://sys.stdout",
            "formatter": "json",
            "filters": ["no_client_address"],
        }
    },
    "loggers": {
        LOGGER_NAME: {"handlers": ["stdout"], "level": "INFO", "propagate": False},
        "uvicorn": {"handlers": ["stdout"], "level": "INFO", "propagate": False},
        "uvicorn.error": {"level": "INFO"},
        "uvicorn.access": {"handlers": [], "level": "CRITICAL", "propagate": False},
        "alembic": {"handlers": ["stdout"], "level": "INFO", "propagate": False},
        "sqlalchemy": {"handlers": ["stdout"], "level": "WARNING", "propagate": False},
    },
    "root": {"handlers": ["stdout"], "level": "WARNING"},
}


def configure_logging() -> None:
    """Route this app's, uvicorn's and alembic's logs through the JSON handler."""
    logging.config.dictConfig(copy.deepcopy(LOGGING_CONFIG))


def log_event(event: str, /, level: int = logging.INFO, **fields: Any) -> None:
    logger.log(level, event, extra={"fields": fields})
