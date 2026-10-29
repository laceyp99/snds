from __future__ import annotations

import json
import os
import shutil
import uuid
from pathlib import Path

import pytest

from snds_worker.client import RunResult, make_request, run_worker
from snds_worker.model import CHECKPOINT_FILE, CHECKPOINT_SIZE, MODEL_NAME

FAKE_ENV = {"SNDS_WORKER_TEST_SEPARATOR": "fake"}
TEST_ENV_VARS = (
    "SNDS_WORKER_TEST_SEPARATOR",
    "SNDS_WORKER_TEST_SEGMENT_DELAY",
    "SNDS_WORKER_TEST_FAULT",
)


def _default_checkpoint() -> Path | None:
    base = os.environ.get("LOCALAPPDATA")
    override = os.environ.get("SNDS_DATA_DIR", "").strip().strip('"')
    root = (
        Path(override)
        if override and os.path.isabs(override)
        else (Path(base) / "snds" if base else None)
    )
    if root is None:
        return None
    p = root / "models" / MODEL_NAME / CHECKPOINT_FILE
    return p if p.is_file() and p.stat().st_size == CHECKPOINT_SIZE else None


REAL_CHECKPOINT = _default_checkpoint()


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    need_cuda = any(item.get_closest_marker("cuda") for item in items)
    cuda_ok = False
    if need_cuda:
        try:
            import torch

            cuda_ok = bool(torch.cuda.is_available())
        except ImportError:
            cuda_ok = False
    for item in items:
        if item.get_closest_marker("model") and REAL_CHECKPOINT is None:
            item.add_marker(pytest.mark.skip(reason="pinned htdemucs checkpoint not installed"))
        if item.get_closest_marker("cuda") and not cuda_ok:
            item.add_marker(pytest.mark.skip(reason="no usable CUDA device"))


@pytest.fixture(autouse=True)
def _clean_test_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Child workers inherit os.environ; never leak test seams or a data-root override."""
    for name in (*TEST_ENV_VARS, "SNDS_DATA_DIR"):
        monkeypatch.delenv(name, raising=False)


@pytest.fixture
def data_root(tmp_path: Path) -> Path:
    root = tmp_path / "data"
    root.mkdir()
    return root


def link_model(root: Path) -> Path:
    """Hard-link (same volume) or copy the real checkpoint into a test data root."""
    assert REAL_CHECKPOINT is not None
    d = root / "models" / MODEL_NAME
    d.mkdir(parents=True, exist_ok=True)
    dst = d / CHECKPOINT_FILE
    if not dst.exists():
        try:
            os.link(REAL_CHECKPOINT, dst)
        except OSError:
            shutil.copyfile(REAL_CHECKPOINT, dst)
    return dst


@pytest.fixture
def model_root(data_root: Path) -> Path:
    link_model(data_root)
    return data_root


def new_job_id() -> str:
    return "t" + uuid.uuid4().hex[:20]


def separate(
    source: Path,
    data_root: Path,
    *,
    env: dict[str, str] | None = None,
    device: str = "auto",
    job_id: str | None = None,
    request: dict | str | None = None,
    **kwargs,
) -> RunResult:
    req = (
        request
        if request is not None
        else make_request(source, job_id or new_job_id(), device, data_root)
    )
    return run_worker("separate", request=req, env=env, **kwargs)


def entry_dir(data_root: Path, result: RunResult) -> Path:
    return Path(result.terminal["cache_dir"])


def load_manifest(entry: Path) -> dict:
    return json.loads((entry / "manifest.json").read_text(encoding="utf-8"))
