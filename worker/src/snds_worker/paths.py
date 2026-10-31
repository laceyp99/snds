"""SNDS data directory layout. The app resolves the same locations the same way."""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

HOME_ENV_VAR = "SNDS_HOME"


class HomeUnavailableError(RuntimeError):
    """Neither SNDS_HOME nor LOCALAPPDATA gives a usable data directory."""


@dataclass(frozen=True)
class DataPaths:
    home: Path

    @property
    def settings_file(self) -> Path:
        return self.home / "settings.json"

    @property
    def cache(self) -> Path:
        return self.home / "cache"

    @property
    def jobs(self) -> Path:
        return self.home / "jobs"

    @property
    def logs(self) -> Path:
        return self.home / "logs"

    @property
    def models(self) -> Path:
        return self.home / "models"


def resolve_home(environ: Mapping[str, str] | None = None) -> Path:
    """Returns SNDS_HOME if set (relative paths resolve against the working directory),
    otherwise %LOCALAPPDATA%\\snds. Nothing is created on disk."""
    env = os.environ if environ is None else environ

    override = env.get(HOME_ENV_VAR, "").strip().strip('"').strip()
    if override:
        return Path(os.path.abspath(override))

    local_app_data = env.get("LOCALAPPDATA", "").strip()
    if not local_app_data:
        raise HomeUnavailableError(f"Neither {HOME_ENV_VAR} nor LOCALAPPDATA is set.")

    return Path(local_app_data) / "snds"


def data_paths(environ: Mapping[str, str] | None = None) -> DataPaths:
    return DataPaths(resolve_home(environ))
