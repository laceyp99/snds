from __future__ import annotations

import logging
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


def test_stderr_only_logging_writes_no_files(
    snds_home: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)
    configure_logging()
    logging.getLogger("snds_worker.test").warning("stderr only")
    assert list(tmp_path.iterdir()) == []
