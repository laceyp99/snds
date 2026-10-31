from __future__ import annotations

import hashlib
import json
import os
import subprocess
from importlib import metadata
from pathlib import Path

import pytest

from snds_worker import model, readiness
from snds_worker.paths import DataPaths

REAL_PROBE_CUDA = readiness.probe_cuda

TOP_LEVEL_KEYS = {
    "protocol",
    "type",
    "ready",
    "worker",
    "packages",
    "compute",
    "model",
    "bpm_model",
    "paths",
    "problems",
    "elapsed_ms",
}


def codes(report: dict) -> list[str]:
    return [entry["code"] for entry in report["problems"]]


@pytest.fixture(autouse=True)
def no_cuda_probe(monkeypatch: pytest.MonkeyPatch) -> None:
    """The CUDA health check starts a child process; tests opt in explicitly."""
    monkeypatch.setattr(readiness, "probe_cuda", lambda: {"ok": False, "ms": 0, "error": "skipped"})


@pytest.fixture
def fake_checkpoints(monkeypatch: pytest.MonkeyPatch) -> dict[str, bytes]:
    """Replaces the pinned size and hash of both checkpoints with those of small fake content."""
    contents = {"MODEL": b"fake htdemucs\n" * 100, "BEAT_MODEL": b"fake beat_this\n" * 100}
    for prefix, content in contents.items():
        monkeypatch.setattr(model, f"{prefix}_SIZE", len(content))
        monkeypatch.setattr(model, f"{prefix}_SHA256", hashlib.sha256(content).hexdigest())
    return contents


def install(path: Path, content: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return path


def install_all(home: Path, contents: dict[str, bytes]) -> tuple[Path, Path]:
    paths = DataPaths(home)
    return (
        install(model.model_path(paths), contents["MODEL"]),
        install(model.beat_model_path(paths), contents["BEAT_MODEL"]),
    )


def test_report_schema_when_checkpoints_are_missing(snds_home: Path) -> None:
    report = readiness.check()

    assert set(report) == TOP_LEVEL_KEYS
    assert report["protocol"] == 1
    assert report["type"] == "readiness"
    assert report["ready"] is False
    assert codes(report) == ["model_missing", "bpm_model_missing"]
    assert all(entry["message"] for entry in report["problems"])
    assert all("bootstrap.ps1" in entry["message"] for entry in report["problems"])

    assert report["worker"]["python"].startswith("3.11.")
    assert report["packages"]["torch"].startswith("2.1.2")
    assert report["packages"]["demucs"] == "4.0.1"
    assert report["packages"]["beat-this"] == "1.1.0"
    assert report["compute"]["cpu_available"] is True
    assert report["compute"]["torch_build"] in {"cpu", "cuda"}
    assert report["compute"]["device"] in {"cpu", "cuda"}
    assert report["model"]["present"] is False
    assert report["model"]["expected_size_bytes"] == model.MODEL_SIZE
    assert report["bpm_model"]["present"] is False
    assert report["bpm_model"]["expected_sha256"] == model.BEAT_MODEL_SHA256
    assert report["paths"]["home"] == str(snds_home)

    json.dumps(report)


def test_readiness_is_read_only(snds_home: Path) -> None:
    readiness.check()
    assert not snds_home.exists()


def test_verified_checkpoints_are_ready(
    snds_home: Path, fake_checkpoints: dict[str, bytes]
) -> None:
    model_file, beat_file = install_all(snds_home, fake_checkpoints)

    report = readiness.check()

    assert report["problems"] == []
    assert report["ready"] is True
    assert report["model"]["present"] is True
    assert report["model"]["path"] == str(model_file)
    assert report["model"]["size_bytes"] == model.MODEL_SIZE
    assert report["model"]["sha256_checked"] is True
    assert report["model"]["verified"] is True
    assert report["bpm_model"]["path"] == str(beat_file)
    assert report["bpm_model"]["verified"] is True


def test_truncated_model_is_reported(snds_home: Path, fake_checkpoints: dict[str, bytes]) -> None:
    model_file, _ = install_all(snds_home, fake_checkpoints)
    model_file.write_bytes(b"x" * 1234)

    report = readiness.check()

    assert codes(report) == ["model_size_mismatch"]
    assert report["ready"] is False
    assert report["model"]["present"] is True
    assert report["model"]["size_bytes"] == 1234
    assert report["model"]["verified"] is False


def test_same_size_but_wrong_content_fails_the_hash_check(
    snds_home: Path, fake_checkpoints: dict[str, bytes]
) -> None:
    model_file, beat_file = install_all(snds_home, fake_checkpoints)
    model_file.write_bytes(b"y" * model.MODEL_SIZE)
    beat_file.write_bytes(b"z" * model.BEAT_MODEL_SIZE)

    report = readiness.check()

    assert codes(report) == ["model_hash_mismatch", "bpm_model_hash_mismatch"]
    assert report["model"]["sha256"] == hashlib.sha256(b"y" * model.MODEL_SIZE).hexdigest()
    assert report["model"]["verified"] is False


def test_missing_bpm_model_is_reported_alone(
    snds_home: Path, fake_checkpoints: dict[str, bytes]
) -> None:
    _, beat_file = install_all(snds_home, fake_checkpoints)
    beat_file.unlink()

    report = readiness.check()

    assert codes(report) == ["bpm_model_missing"]
    assert report["model"]["verified"] is True
    assert report["bpm_model"]["present"] is False


def test_unreadable_checkpoint_is_reported(
    snds_home: Path, fake_checkpoints: dict[str, bytes], monkeypatch: pytest.MonkeyPatch
) -> None:
    install_all(snds_home, fake_checkpoints)

    def locked(_path: Path) -> str:
        raise PermissionError("locked")

    monkeypatch.setattr(model, "sha256_file", locked)

    report = readiness.check()

    assert codes(report) == ["model_unreadable", "bpm_model_unreadable"]


@pytest.mark.parametrize(
    ("probe", "healthy", "device"),
    [
        ({"ok": True, "ms": 5}, True, "cuda"),
        ({"ok": False, "ms": 5, "error": "boom"}, False, "cpu"),
    ],
)
def test_cuda_health_decides_the_auto_device(
    snds_home: Path, monkeypatch: pytest.MonkeyPatch, probe: dict, healthy: bool, device: str
) -> None:
    import torch

    monkeypatch.setattr(torch.cuda, "is_available", lambda: True)
    monkeypatch.setattr(torch.cuda, "get_device_name", lambda _index: "Fake GPU")
    monkeypatch.setattr(torch.version, "cuda", "12.1")
    monkeypatch.setattr(readiness, "probe_cuda", lambda: probe)

    compute = readiness.check()["compute"]

    assert compute["cuda_available"] is True
    assert compute["cuda_healthy"] is healthy
    assert compute["device"] == device
    assert compute["cuda_device"] == "Fake GPU"
    assert compute.get("cuda_error") == probe.get("error")


def test_check_cuda_false_skips_the_probe(snds_home: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    import torch

    monkeypatch.setattr(torch.cuda, "is_available", lambda: True)
    monkeypatch.setattr(torch.version, "cuda", "12.1")
    monkeypatch.setattr(torch.cuda, "get_device_name", lambda _index: "Fake GPU")
    monkeypatch.setattr(readiness, "probe_cuda", lambda: pytest.fail("probe must not run"))

    compute = readiness.check(check_cuda=False)["compute"]

    assert compute["cuda_available"] is True
    assert compute["cuda_healthy"] is False
    assert compute["device"] == "cpu"


def test_probe_cuda_reports_timeouts_and_failures(monkeypatch: pytest.MonkeyPatch) -> None:
    def timed_out(*_args, **_kwargs):
        raise subprocess.TimeoutExpired("python", 1)

    monkeypatch.setattr(readiness.subprocess, "run", timed_out)
    result = REAL_PROBE_CUDA(timeout=1)
    assert result["ok"] is False
    assert "timed out" in result["error"]

    def crashed(*_args, **_kwargs):
        return subprocess.CompletedProcess([], 1, "", "Traceback\nRuntimeError: CUDA error\n")

    monkeypatch.setattr(readiness.subprocess, "run", crashed)
    assert REAL_PROBE_CUDA()["error"] == "RuntimeError: CUDA error"

    def passed(*_args, **_kwargs):
        return subprocess.CompletedProcess([], 0, '{"ok": true}\n', "")

    monkeypatch.setattr(readiness.subprocess, "run", passed)
    assert REAL_PROBE_CUDA()["ok"] is True


@pytest.mark.skipif(
    os.environ.get("SNDS_RUN_CUDA_TESTS") != "1", reason="set SNDS_RUN_CUDA_TESTS=1 for a real GPU"
)
def test_real_cuda_probe() -> None:
    assert REAL_PROBE_CUDA()["ok"] is True


def test_import_failure_is_reported_with_details(
    snds_home: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setitem(readiness._IMPORTS, "soundfile", "snds_worker_missing_module")

    report = readiness.check()

    failure = next(entry for entry in report["problems"] if entry["code"] == "import_failed")
    assert "snds_worker_missing_module" in failure["message"]
    assert "Traceback" in failure["details"]


def test_missing_package_is_reported(snds_home: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    real_version = metadata.version

    def fake_version(name: str) -> str:
        if name in {"demucs", "beat-this"}:
            raise metadata.PackageNotFoundError(name)
        return real_version(name)

    monkeypatch.setattr(readiness.metadata, "version", fake_version)

    report = readiness.check()

    assert codes(report).count("package_missing") == 2
    assert report["packages"]["demucs"] is None
    assert report["packages"]["beat-this"] is None


def test_unresolvable_home_is_reported() -> None:
    report = readiness.check({})

    assert codes(report) == ["home_unavailable"]
    assert report["paths"] is None
    assert report["model"]["path"] is None
    assert report["bpm_model"]["path"] is None
