from __future__ import annotations

import hashlib
import http.client
import io
from pathlib import Path

import pytest

from snds_worker import model

CONTENT = b"not really a checkpoint\n" * 1000
SHA256 = hashlib.sha256(CONTENT).hexdigest()


@pytest.fixture
def source(tmp_path: Path) -> str:
    path = tmp_path / "remote" / "checkpoint.th"
    path.parent.mkdir()
    path.write_bytes(CONTENT)
    return path.as_uri()


def leftovers(folder: Path) -> list[Path]:
    return list(folder.glob("*.part"))


def test_download_verifies_and_installs(tmp_path: Path, source: str) -> None:
    destination = tmp_path / "models" / "htdemucs" / "x.th"

    result = model.fetch(destination, source, SHA256, len(CONTENT))

    assert result == model.FetchResult(destination, True, len(CONTENT), SHA256)
    assert destination.read_bytes() == CONTENT
    assert leftovers(destination.parent) == []


def test_hash_mismatch_leaves_nothing_behind(tmp_path: Path, source: str) -> None:
    destination = tmp_path / "models" / "x.th"

    with pytest.raises(model.ModelVerificationError, match="SHA-256"):
        model.fetch(destination, source, "0" * 64, len(CONTENT))

    assert not destination.exists()
    assert leftovers(destination.parent) == []


def test_size_mismatch_is_rejected(tmp_path: Path, source: str) -> None:
    destination = tmp_path / "x.th"

    with pytest.raises(model.ModelVerificationError):
        model.fetch(destination, source, SHA256, len(CONTENT) + 1)

    assert not destination.exists()
    assert leftovers(tmp_path) == []


def test_verified_existing_file_is_not_downloaded_again(tmp_path: Path) -> None:
    destination = tmp_path / "x.th"
    destination.write_bytes(CONTENT)
    unreachable = (tmp_path / "does-not-exist.th").as_uri()

    result = model.fetch(destination, unreachable, SHA256, len(CONTENT))

    assert not result.downloaded
    assert destination.read_bytes() == CONTENT


def test_corrupt_existing_file_is_replaced(tmp_path: Path, source: str) -> None:
    destination = tmp_path / "x.th"
    destination.write_bytes(b"truncated")

    result = model.fetch(destination, source, SHA256, len(CONTENT))

    assert result.downloaded
    assert destination.read_bytes() == CONTENT


def test_failed_download_keeps_existing_file(tmp_path: Path) -> None:
    destination = tmp_path / "x.th"
    destination.write_bytes(b"old")
    unreachable = (tmp_path / "does-not-exist.th").as_uri()

    with pytest.raises(OSError):
        model.fetch(destination, unreachable, SHA256, len(CONTENT))

    assert destination.read_bytes() == b"old"
    assert leftovers(tmp_path) == []


def test_pinned_checkpoint_constants() -> None:
    assert model.MODEL_FILENAME.startswith(f"{model.MODEL_SIGNATURE}-{model.MODEL_SHA256[:8]}")
    assert model.MODEL_URL.endswith("/" + model.MODEL_FILENAME)
    assert len(model.MODEL_SHA256) == 64


@pytest.mark.parametrize("failure", ["short", "http"])
def test_interrupted_download_is_retried(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: str
) -> None:
    calls = 0

    def open_response(*args: object, **kwargs: object) -> io.BytesIO:
        nonlocal calls
        calls += 1
        if calls == 1 and failure == "http":
            raise http.client.RemoteDisconnected("closed early")
        response = io.BytesIO(CONTENT[:10] if calls == 1 else CONTENT)
        response.headers = {}  # type: ignore[attr-defined]
        return response

    monkeypatch.setattr(model.urllib.request, "urlopen", open_response)
    monkeypatch.setattr(model.time, "sleep", lambda _: None)
    destination = tmp_path / "x.th"

    assert model.fetch(destination, "https://example.test/model", SHA256, len(CONTENT)).downloaded
    assert calls == 2
    assert destination.read_bytes() == CONTENT
    assert leftovers(tmp_path) == []


def test_truncated_download_exhausts_retries_without_replacing_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls = 0

    def open_response(*args: object, **kwargs: object) -> io.BytesIO:
        nonlocal calls
        calls += 1
        response = io.BytesIO(CONTENT[:10])
        response.headers = {}  # type: ignore[attr-defined]
        return response

    monkeypatch.setattr(model.urllib.request, "urlopen", open_response)
    monkeypatch.setattr(model.time, "sleep", lambda _: None)
    destination = tmp_path / "x.th"
    destination.write_bytes(b"old")

    with pytest.raises(http.client.IncompleteRead):
        model.fetch(destination, "https://example.test/model", SHA256, len(CONTENT))
    assert calls == 3
    assert destination.read_bytes() == b"old"
    assert leftovers(tmp_path) == []
