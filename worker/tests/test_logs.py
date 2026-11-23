from __future__ import annotations

import logging
import os
from pathlib import Path

import pytest

from snds_worker.logs import BACKUP_COUNT, LOG_FILE_NAME, configure_logging, redact

ENV = {
    "USERPROFILE": r"C:\Users\Pat",
    "LOCALAPPDATA": r"C:\Users\Pat\AppData\Local",
}


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        (r"C:\Users\Pat\Music\song.mp3", r"%USERPROFILE%\Music\song.mp3"),
        (r"c:\users\PAT\music", r"%USERPROFILE%\music"),
        ("C:/Users/Pat/Music", "%USERPROFILE%/Music"),
        (r"C:\Users\Pat\AppData\Local\snds\logs", r"%LOCALAPPDATA%\snds\logs"),
        (r'{"path": "C:\\Users\\Pat\\AppData\\Local\\snds"}', r'{"path": "%LOCALAPPDATA%\\snds"}'),
        (r"home is C:\Users\Pat.", r"home is %USERPROFILE%."),
        (r"C:\Users\Patty\Music", r"C:\Users\Patty\Music"),
        (r"D:\Music\song.mp3", r"D:\Music\song.mp3"),
    ],
)
def test_redact(text: str, expected: str) -> None:
    assert redact(text, ENV) == expected


def test_redact_ignores_unset_variables() -> None:
    assert redact(r"C:\Users\Pat\x", {}) == r"C:\Users\Pat\x"


def test_file_log_is_redacted_and_rotates(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    for name, value in ENV.items():
        monkeypatch.setenv(name, value)

    log_dir = tmp_path / "logs"
    configure_logging(log_dir, max_bytes=2000)
    log = logging.getLogger("snds_worker.test")

    for index in range(400):  # about 32 KB: enough to hit the backup cap
        log.info(r"line %03d C:\Users\Pat\AppData\Local\snds\cache", index)

    current = log_dir / LOG_FILE_NAME
    assert current.is_file()
    assert r"%LOCALAPPDATA%\snds\cache" in current.read_text(encoding="utf-8")
    assert r"C:\Users\Pat" not in current.read_text(encoding="utf-8")

    rotated = sorted(log_dir.glob(f"{LOG_FILE_NAME}.*"))
    assert len(rotated) == BACKUP_COUNT
    assert all(path.stat().st_size <= 2000 for path in rotated)


@pytest.mark.skipif(os.name != "nt", reason="Windows read handles deny log rename")
def test_locked_active_log_skips_record_without_shifting_archives(tmp_path: Path) -> None:
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    (log_dir / LOG_FILE_NAME).write_text("active", encoding="utf-8")
    (log_dir / f"{LOG_FILE_NAME}.1").write_text("archive one", encoding="utf-8")
    (log_dir / f"{LOG_FILE_NAME}.2").write_text("archive two", encoding="utf-8")
    configure_logging(log_dir, max_bytes=1)

    log = logging.getLogger("snds_worker.locked")
    with (log_dir / LOG_FILE_NAME).open("rb"):
        log.info("skipped while locked")

    assert (log_dir / LOG_FILE_NAME).read_text(encoding="utf-8") == "active"
    assert (log_dir / f"{LOG_FILE_NAME}.1").read_text(encoding="utf-8") == "archive one"
    assert (log_dir / f"{LOG_FILE_NAME}.2").read_text(encoding="utf-8") == "archive two"

    log.info("written after unlock")

    contents = "\n".join(
        path.read_text(encoding="utf-8") for path in log_dir.glob(f"{LOG_FILE_NAME}*")
    )
    assert "skipped while locked" not in contents
    assert "active" in contents
    assert "archive one" in contents
    assert "archive two" in contents
    assert "written after unlock" in contents


def test_interrupted_rotation_is_archived_before_active_log(tmp_path: Path) -> None:
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    (log_dir / LOG_FILE_NAME).write_text("active", encoding="utf-8")
    (log_dir / f"{LOG_FILE_NAME}.rotating").write_text("pending", encoding="utf-8")
    (log_dir / f"{LOG_FILE_NAME}.1").write_text("archive one", encoding="utf-8")
    configure_logging(log_dir, max_bytes=1)

    logging.getLogger("snds_worker.recovery").info("new record")

    assert (log_dir / f"{LOG_FILE_NAME}.1").read_text(encoding="utf-8") == "active"
    assert (log_dir / f"{LOG_FILE_NAME}.2").read_text(encoding="utf-8") == "pending"
    assert (log_dir / f"{LOG_FILE_NAME}.3").read_text(encoding="utf-8") == "archive one"
    assert "new record" in (log_dir / LOG_FILE_NAME).read_text(encoding="utf-8")


def test_stderr_only_logging_writes_no_files(
    snds_home: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)
    configure_logging()
    logging.getLogger("snds_worker.test").warning("stderr only")
    assert list(tmp_path.iterdir()) == []
