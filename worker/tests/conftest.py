from __future__ import annotations

import logging
import os
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Literal

import pytest

from snds_worker import model
from snds_worker.paths import data_paths


@pytest.fixture
def real_checkpoint() -> Callable[[Literal["htdemucs", "beat_this"]], Path]:
    """Resolve installed checkpoints only when real-model tests are explicitly enabled."""
    if os.environ.get("SNDS_RUN_MODEL_TESTS") != "1":
        pytest.skip("set SNDS_RUN_MODEL_TESTS=1 to run real checkpoint tests")

    def resolve(name: Literal["htdemucs", "beat_this"]) -> Path:
        if name == "htdemucs":
            override = os.environ.get("SNDS_TEST_MODEL_DIR")
            path = (
                Path(override) / model.MODEL_FILENAME
                if override
                else model.model_path(data_paths())
            )
        else:
            path = model.beat_model_path(data_paths())
        if not path.is_file():
            pytest.fail(f"SNDS_RUN_MODEL_TESTS=1 requires the {name} checkpoint at {path}")
        return path

    return resolve


@pytest.fixture(autouse=True)
def no_network(monkeypatch: pytest.MonkeyPatch) -> None:
    """Allow file:// downloads only; tests must never reach the network."""
    real_urlopen = urllib.request.urlopen

    def guarded_urlopen(request, *args, **kwargs):
        url = request.full_url if isinstance(request, urllib.request.Request) else str(request)
        if not url.startswith("file:"):
            raise AssertionError(f"tests must not access the network: {url}")
        return real_urlopen(request, *args, **kwargs)

    monkeypatch.setattr(urllib.request, "urlopen", guarded_urlopen)


@pytest.fixture(autouse=True)
def reset_worker_logging():
    """configure_logging() installs handlers on the root logger; remove them after each test so
    that log files in temporary directories are closed."""
    yield
    root = logging.getLogger()
    for handler in [h for h in root.handlers if getattr(h, "_snds_worker_handler", False)]:
        root.removeHandler(handler)
        handler.close()


@pytest.fixture
def snds_home(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Points SNDS_HOME at a fresh, not yet created directory."""
    home = tmp_path / "snds-home"
    monkeypatch.setenv("SNDS_HOME", str(home))
    return home


@pytest.fixture
def home(tmp_path: Path) -> Path:
    return tmp_path / "snds-home"


@pytest.fixture
def started(home: Path):
    """Starts `prepare` worker subprocesses (fake separator) and kills any left running."""
    from procs import Job

    jobs: list[Job] = []

    def start(env: dict[str, str] | None = None, argv: list[str] | None = None) -> Job:
        job = Job(home, env, argv)
        jobs.append(job)
        return job

    yield start
    for job in jobs:
        job.kill()
