"""A worker subprocess driven like the app drives it: pipes, a request line, NDJSON events."""

from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
from pathlib import Path

from helpers import make_request, write_wav
from snds_worker import jobs_protocol

TIMEOUT = 120


class Job:
    """A running worker process with its events collected on a background thread."""

    def __init__(
        self, home: Path, env: dict[str, str] | None = None, argv: list[str] | None = None
    ) -> None:
        self.stderr_path = home.parent / "stderr.log"
        environment = {
            **os.environ,
            "SNDS_HOME": str(home),
            "SNDS_FAKE_SEPARATOR": "1",
            **(env or {}),
        }
        self.process = subprocess.Popen(
            argv or [sys.executable, "-I", "-m", "snds_worker", "prepare"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_path.open("wb"),
            env=environment,
        )
        self.lines: queue.Queue[bytes | None] = queue.Queue()
        self.raw: list[bytes] = []
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self) -> None:
        assert self.process.stdout is not None
        try:
            for line in self.process.stdout:
                self.lines.put(line)
        except (OSError, ValueError):  # a test closed our end of the pipe
            pass
        self.lines.put(None)

    def send(self, data: bytes | dict) -> None:
        assert self.process.stdin is not None
        if isinstance(data, dict):
            data = json.dumps(data).encode("ascii") + b"\n"
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def close_stdin(self) -> None:
        assert self.process.stdin is not None
        self.process.stdin.close()

    def next_event(self) -> dict | None:
        line = self.lines.get(timeout=TIMEOUT)
        if line is None:
            return None
        self.raw.append(line)
        return json.loads(line)

    def events_until(self, predicate) -> list[dict]:
        """Reads events up to and including the first that satisfies the predicate."""
        events = []
        while (event := self.next_event()) is not None:
            events.append(event)
            if predicate(event):
                break
        return events

    def finish(self) -> tuple[list[dict], int]:
        events = []
        while (event := self.next_event()) is not None:
            events.append(event)
        return events, self.process.wait(timeout=TIMEOUT)

    @property
    def stderr(self) -> str:
        return self.stderr_path.read_text("utf-8", errors="replace")

    def kill(self) -> None:
        if self.process.poll() is None:
            self.process.kill()


def request_for_source(source: Path, tmp_path: Path) -> dict:
    request = make_request(tmp_path, source)
    return {
        "protocol": 1,
        "type": "prepare",
        "jobId": request.job_id,
        "source": {
            "path": request.source.path,
            "sizeBytes": request.source.size_bytes,
            "sha256": request.source.sha256,
        },
        "jobDir": request.job_dir,
        "cacheDir": request.cache_dir,
        "compute": {"preference": "cpu"},
    }


def request_for(tmp_path: Path, seconds: float = 4.0, **kwargs) -> dict:
    source = write_wav(tmp_path / "song.wav", 44_100, seconds)
    return request_for_source(source, tmp_path)


def check_stream(raw: list[bytes], events: list[dict]) -> None:
    """Every stdout line is one pure-ASCII JSON event with contiguous seq numbers."""
    for line in raw:
        line.decode("ascii")
        assert line.endswith(b"\n")
    assert [e["seq"] for e in events] == list(range(1, len(events) + 1))
    assert all(e["protocol"] == 1 for e in events)
    terminals = [e for e in events if e["type"] in jobs_protocol.TERMINAL_TYPES]
    assert terminals == [events[-1]]
