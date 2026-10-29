"""Human-readable logging to stderr (the app drains it into its rotating worker log)."""

from __future__ import annotations

import logging
import os
import sys
import time

LOG_FORMAT = "%(asctime)s.%(msecs)03dZ %(levelname)s %(name)s: %(message)s"
LOG_LEVEL_ENV = "SNDS_WORKER_LOG_LEVEL"


def make_formatter() -> logging.Formatter:
    fmt = logging.Formatter(LOG_FORMAT, datefmt="%Y-%m-%dT%H:%M:%S")
    fmt.converter = time.gmtime
    return fmt


def configure_logging() -> None:
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(make_formatter())
    root.addHandler(handler)
    level = os.environ.get(LOG_LEVEL_ENV, "INFO").upper()
    root.setLevel(level if level in logging.getLevelNamesMapping() else "INFO")
