"""SNDS per-user data directories. Mirrors ``snds::AppPaths`` (src/app/AppPaths.cpp) and
``Get-SndsDataRoot`` (scripts/_common.ps1): an absolute ``SNDS_DATA_DIR`` wins, otherwise
``%LOCALAPPDATA%\\snds``. Empty or relative override values are ignored.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from snds_worker.protocol import ErrorCode, WorkerError

DATA_ROOT_ENV = "SNDS_DATA_DIR"
ROOT_FOLDER = "snds"


def resolve_data_root(explicit: Path | str | None = None) -> Path:
    if explicit is not None:
        path = Path(explicit)
        if not path.is_absolute():
            raise WorkerError(
                ErrorCode.INVALID_REQUEST,
                "The data root must be an absolute path",
                {"value": str(explicit)},
            )
        return path
    override = os.environ.get(DATA_ROOT_ENV, "").strip().strip('"').strip()
    if override and os.path.isabs(override):
        return Path(override)
    base = os.environ.get("LOCALAPPDATA", "").strip()
    if not base:
        raise WorkerError(
            ErrorCode.DATA_DIR_UNAVAILABLE,
            "LOCALAPPDATA is not set and SNDS_DATA_DIR is not an absolute path",
        )
    return Path(base) / ROOT_FOLDER


@dataclass(frozen=True)
class DataDirs:
    root: Path

    @classmethod
    def resolve(cls, explicit: Path | str | None = None) -> DataDirs:
        return cls(resolve_data_root(explicit))

    @property
    def settings(self) -> Path:
        return self.root / "settings"

    @property
    def cache(self) -> Path:
        return self.root / "cache"

    @property
    def jobs(self) -> Path:
        return self.root / "jobs"

    @property
    def logs(self) -> Path:
        return self.root / "logs"

    @property
    def models(self) -> Path:
        return self.root / "models"

    def ensure(self, *dirs: Path) -> None:
        for d in dirs:
            try:
                d.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                raise WorkerError(
                    ErrorCode.DATA_DIR_UNAVAILABLE,
                    f"Cannot create {d}: {exc.strerror or exc}",
                    {"path": str(d)},
                ) from exc
            if not d.is_dir():
                raise WorkerError(
                    ErrorCode.DATA_DIR_UNAVAILABLE, f"{d} is not a directory", {"path": str(d)}
                )

    def same_volume(self, a: Path, b: Path) -> bool:
        return os.stat(a).st_dev == os.stat(b).st_dev
