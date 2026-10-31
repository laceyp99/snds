from __future__ import annotations

import os
from pathlib import Path

import pytest

from snds_worker import model
from snds_worker.paths import DataPaths, HomeUnavailableError, data_paths, resolve_home


def test_snds_home_overrides_local_app_data(tmp_path: Path) -> None:
    env = {"SNDS_HOME": str(tmp_path / "custom"), "LOCALAPPDATA": r"C:\Users\x\AppData\Local"}
    assert resolve_home(env) == tmp_path / "custom"


def test_default_is_local_app_data_snds() -> None:
    env = {"LOCALAPPDATA": r"C:\Users\x\AppData\Local"}
    assert resolve_home(env) == Path(r"C:\Users\x\AppData\Local\snds")


@pytest.mark.parametrize("value", ["", "   ", '""'])
def test_blank_snds_home_falls_back_to_default(value: str) -> None:
    env = {"SNDS_HOME": value, "LOCALAPPDATA": r"C:\Users\x\AppData\Local"}
    assert resolve_home(env) == Path(r"C:\Users\x\AppData\Local\snds")


def test_quoted_and_relative_snds_home_is_made_absolute(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)
    assert resolve_home({"SNDS_HOME": '"data\\..\\home"'}) == tmp_path / "home"


def test_missing_home_and_local_app_data_raises() -> None:
    with pytest.raises(HomeUnavailableError):
        resolve_home({})


def test_layout_matches_the_app_contract(tmp_path: Path) -> None:
    paths = data_paths({"SNDS_HOME": str(tmp_path)})
    assert paths == DataPaths(tmp_path)
    assert paths.settings_file == tmp_path / "settings.json"
    assert paths.cache == tmp_path / "cache"
    assert paths.jobs == tmp_path / "jobs"
    assert paths.logs == tmp_path / "logs"
    assert model.model_path(paths) == tmp_path / "models" / "htdemucs" / "955717e8-8726e21a.th"


def test_resolution_creates_nothing(tmp_path: Path) -> None:
    data_paths({"SNDS_HOME": str(tmp_path / "home")})
    assert os.listdir(tmp_path) == []
