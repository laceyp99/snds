from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from snds_worker import cli, model, protocol


def single_json_line(out: str) -> dict:
    lines = out.splitlines()
    assert len(lines) == 1, out
    return json.loads(lines[0])


@pytest.fixture
def fake_checkpoint(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> bytes:
    """Points both pinned checkpoints at small local files; returns the htdemucs content."""
    (tmp_path / "remote").mkdir()
    contents = {}
    for prefix, filename, content in (
        ("MODEL", model.MODEL_FILENAME, b"fake checkpoint\n" * 512),
        ("BEAT_MODEL", model.BEAT_MODEL_FILENAME, b"fake beat checkpoint\n" * 256),
    ):
        source = tmp_path / "remote" / filename
        source.write_bytes(content)
        monkeypatch.setattr(model, f"{prefix}_URL", source.as_uri())
        monkeypatch.setattr(model, f"{prefix}_SHA256", hashlib.sha256(content).hexdigest())
        monkeypatch.setattr(model, f"{prefix}_SIZE", len(content))
        contents[prefix] = content
    return contents["MODEL"]


def test_readiness_prints_one_json_object(snds_home: Path, capfd: pytest.CaptureFixture) -> None:
    exit_code = cli.main(["readiness"])

    report = single_json_line(capfd.readouterr().out)
    assert report["type"] == "readiness"
    assert exit_code == cli.EXIT_FAILED  # no checkpoint in a fresh SNDS_HOME
    assert not snds_home.exists()


def test_stray_output_is_moved_to_stderr(
    snds_home: Path, capfd: pytest.CaptureFixture, monkeypatch: pytest.MonkeyPatch
) -> None:
    def noisy_command(_args):
        print("python chatter")
        os.write(1, b"native chatter\n")
        return cli.EXIT_OK, protocol.message("test")

    monkeypatch.setitem(cli.COMMANDS, "readiness", noisy_command)

    exit_code = cli.main(["readiness"])

    captured = capfd.readouterr()
    assert exit_code == cli.EXIT_OK
    assert single_json_line(captured.out) == {"protocol": 1, "type": "test"}
    assert "python chatter" in captured.err
    assert "native chatter" in captured.err


def test_fetch_model_installs_into_snds_home(
    snds_home: Path, fake_checkpoint: bytes, capfd: pytest.CaptureFixture
) -> None:
    exit_code = cli.main(["fetch-model"])

    result = single_json_line(capfd.readouterr().out)
    destination = snds_home / "models" / "htdemucs" / model.MODEL_FILENAME
    assert exit_code == cli.EXIT_OK
    assert result["ok"] is True
    assert result["downloaded"] is True
    assert Path(result["path"]) == destination
    assert destination.read_bytes() == fake_checkpoint
    beat_destination = snds_home / "models" / "beat_this" / model.BEAT_MODEL_FILENAME
    assert beat_destination.is_file()
    assert [entry["name"] for entry in result["models"]] == ["htdemucs", "beat_this"]
    assert Path(result["models"][1]["path"]) == beat_destination
    assert "Checkpoint verified" in (snds_home / "logs" / "worker.log").read_text("utf-8")

    exit_code = cli.main(["fetch-model"])

    again = single_json_line(capfd.readouterr().out)
    assert exit_code == cli.EXIT_OK
    assert again["downloaded"] is False


def test_fetch_model_reports_verification_failure(
    snds_home: Path,
    fake_checkpoint: bytes,
    capfd: pytest.CaptureFixture,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(model, "MODEL_SHA256", "0" * 64)

    exit_code = cli.main(["fetch-model"])

    result = single_json_line(capfd.readouterr().out)
    assert exit_code == cli.EXIT_FAILED
    assert result["ok"] is False
    assert result["problems"][0]["code"] == "model_verification_failed"
    assert not (snds_home / "models" / "htdemucs" / model.MODEL_FILENAME).exists()
    # The other checkpoint is still fetched and reported.
    assert [entry["name"] for entry in result["models"]] == ["beat_this"]


def test_fetch_model_reports_beat_checkpoint_failure(
    snds_home: Path,
    fake_checkpoint: bytes,
    capfd: pytest.CaptureFixture,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(model, "BEAT_MODEL_SHA256", "0" * 64)

    exit_code = cli.main(["fetch-model"])

    result = single_json_line(capfd.readouterr().out)
    assert exit_code == cli.EXIT_FAILED
    assert result["ok"] is False
    assert "beat_this" in result["problems"][0]["message"]
    assert result["downloaded"] is True  # htdemucs fields stay at the top level
    assert not (snds_home / "models" / "beat_this" / model.BEAT_MODEL_FILENAME).exists()


def test_fetch_model_reports_download_failure(
    snds_home: Path,
    fake_checkpoint: bytes,
    tmp_path: Path,
    capfd: pytest.CaptureFixture,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(model, "MODEL_URL", (tmp_path / "missing.th").as_uri())

    exit_code = cli.main(["fetch-model"])

    result = single_json_line(capfd.readouterr().out)
    assert exit_code == cli.EXIT_FAILED
    assert result["problems"][0]["code"] == "download_failed"


def test_http_failure_is_reported_as_download_failure(
    snds_home: Path, capfd: pytest.CaptureFixture, monkeypatch: pytest.MonkeyPatch
) -> None:
    import http.client

    def interrupted(*args: object, **kwargs: object) -> None:
        raise http.client.IncompleteRead(b"", 10)

    monkeypatch.setattr(model, "fetch", interrupted)
    assert cli.main(["fetch-model"]) == cli.EXIT_FAILED
    result = single_json_line(capfd.readouterr().out)
    assert len(result["problems"]) == 2
    assert {problem["code"] for problem in result["problems"]} == {"download_failed"}


def test_unexpected_errors_still_produce_json(
    snds_home: Path, capfd: pytest.CaptureFixture, monkeypatch: pytest.MonkeyPatch
) -> None:
    def broken_command(_args):
        raise RuntimeError("boom")

    monkeypatch.setitem(cli.COMMANDS, "readiness", broken_command)

    exit_code = cli.main(["readiness"])

    captured = capfd.readouterr()
    result = single_json_line(captured.out)
    assert exit_code == cli.EXIT_INTERNAL
    assert result["type"] == "error"
    assert result["problems"][0] == {"code": "internal_error", "message": "RuntimeError: boom"}
    assert "Traceback" in captured.err


@pytest.mark.parametrize("argv", [[], ["no-such-command"]])
def test_usage_errors_exit_2_without_stdout(argv: list[str], capfd: pytest.CaptureFixture) -> None:
    with pytest.raises(SystemExit) as raised:
        cli.main(argv)

    assert raised.value.code == 2
    assert capfd.readouterr().out == ""


def test_module_entry_point(snds_home: Path) -> None:
    # -I (isolated mode) is how the app launches the worker.
    completed = subprocess.run(
        [sys.executable, "-I", "-m", "snds_worker", "readiness"],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )

    report = single_json_line(completed.stdout)
    assert completed.returncode == cli.EXIT_FAILED, completed.stderr
    assert report["problems"][0]["code"] == "model_missing"
    assert "Traceback" not in completed.stderr


def test_runs_without_stdin_or_stderr(snds_home: Path) -> None:
    # The app's launcher (juce::ChildProcess) passes no stdin or stderr handle at all.
    script = (
        "import os, sys; os.close(0); os.close(2); sys.stdin = sys.stderr = None; "
        "from snds_worker.cli import main; raise SystemExit(main(['readiness']))"
    )
    completed = subprocess.run(
        [sys.executable, "-I", "-c", script],
        stdout=subprocess.PIPE,
        text=True,
        timeout=300,
        check=False,
    )

    report = single_json_line(completed.stdout)
    assert report["type"] == "readiness"
    assert completed.returncode == cli.EXIT_FAILED
    assert report["problems"][0]["code"] == "model_missing"
