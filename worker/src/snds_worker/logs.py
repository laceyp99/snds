"""Worker logging: stderr always, plus a rotating worker.log when a command needs one.

Every handler redacts the user's profile and local app data directories, matching the app.
"""

from __future__ import annotations

import logging
import os
import re
import sys
from collections.abc import Mapping
from logging.handlers import RotatingFileHandler
from pathlib import Path

LOG_FILE_NAME = "worker.log"
MAX_BYTES = 1024 * 1024
BACKUP_COUNT = 5

_REDACTED_VARIABLES = ("LOCALAPPDATA", "USERPROFILE")
_OWNED = "_snds_worker_handler"  # marks handlers that configure_logging may replace


def _path_pattern(path: str) -> re.Pattern[str]:
    parts = [re.escape(part) for part in re.split(r"[\\/]+", path) if part]
    # Accept either separator (including doubled backslashes in JSON), and only match whole
    # path components: "C:\Users\Pat" must not redact "C:\Users\Patty".
    return re.compile(r"[\\/]+".join(parts) + r"(?![\w-])", re.IGNORECASE)


def redact(text: str, environ: Mapping[str, str] | None = None) -> str:
    """Replaces %LOCALAPPDATA% and %USERPROFILE% paths (case-insensitive) with the variable name."""
    env = os.environ if environ is None else environ
    replacements = [
        (value, f"%{name}%")
        for name in _REDACTED_VARIABLES
        if (value := env.get(name, "").strip().rstrip("\\/"))
    ]

    # Longest first, so %LOCALAPPDATA% wins over the %USERPROFILE% that usually contains it.
    for value, token in sorted(replacements, key=lambda item: len(item[0]), reverse=True):
        text = _path_pattern(value).sub(lambda _match, token=token: token, text)

    return text


class RedactingFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        return redact(super().format(record))


class _WindowsSafeRotatingFileHandler(RotatingFileHandler):
    """Move the active file first so Windows locks cannot shift existing backups."""

    def handleError(self, record: logging.LogRecord) -> None:
        # Another viewer can deny a rename. The separate stderr handler already emitted
        # the redacted record; logging's default diagnostic would also print its raw args.
        if not isinstance(sys.exc_info()[1], OSError):
            super().handleError(record)

    def doRollover(self) -> None:
        if self.stream:
            self.stream.close()
            self.stream = None

        base = Path(self.baseFilename)
        rotating = Path(f"{base}.rotating")

        def backup(index: int) -> Path:
            return Path(f"{base}.{index}")

        def archive_pending() -> None:
            slot = 1
            while slot < self.backupCount and backup(slot).exists():
                slot += 1
            for index in range(slot, 1, -1):
                os.replace(backup(index - 1), backup(index))
            os.replace(rotating, backup(1))

        if rotating.exists():
            archive_pending()
        base.rename(rotating)
        archive_pending()
        # FileHandler.emit opens a new stream on the next successful write.


def configure_logging(log_dir: Path | None = None, *, max_bytes: int = MAX_BYTES) -> None:
    """Sends log records to stderr, and to <log_dir>/worker.log when log_dir is given.

    Read-only commands pass no log_dir so that they never write into the data directory.
    """
    root = logging.getLogger()
    for handler in [h for h in root.handlers if getattr(h, _OWNED, False)]:
        root.removeHandler(handler)
        handler.close()

    root.setLevel(logging.WARNING)
    logging.getLogger("snds_worker").setLevel(logging.INFO)
    logging.captureWarnings(True)

    stderr_handler = logging.StreamHandler(sys.stderr)
    stderr_handler.setFormatter(RedactingFormatter("%(levelname)s %(message)s"))
    handlers: list[logging.Handler] = [stderr_handler]

    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        file_handler = _WindowsSafeRotatingFileHandler(
            log_dir / LOG_FILE_NAME,
            maxBytes=max_bytes,
            backupCount=BACKUP_COUNT,
            encoding="utf-8",
        )
        file_handler.setFormatter(
            RedactingFormatter("%(asctime)s [%(process)d] %(levelname)s %(name)s: %(message)s")
        )
        handlers.append(file_handler)

    for handler in handlers:
        setattr(handler, _OWNED, True)
        root.addHandler(handler)
