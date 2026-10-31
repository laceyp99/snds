"""`python -I -m snds_worker prepare` as the app runs it: pipes, a request line, NDJSON events."""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from procs import check_stream, request_for
from snds_worker import jobs_protocol


def test_full_event_stream_with_library_chatter(tmp_path: Path, home: Path, started) -> None:
    request = request_for(tmp_path)
    job = started({"SNDS_FAKE_CHATTER": "1"})

    job.send(request)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert [e["type"] for e in events[:2]] == ["accepted", "stage"]
    assert [e["stage"] for e in events if e["type"] == "stage"] == list(jobs_protocol.STAGES)
    assert events[0]["device"] == "cpu"
    assert events[-1]["type"] == "result"
    assert events[-1]["cacheDir"] == request["cacheDir"]
    assert code == jobs_protocol.EXIT_OK
    assert (Path(request["cacheDir"]) / "song.json").is_file()
    assert not (Path(request["jobDir"]) / "package").exists()  # it was renamed into the cache
    assert "chatter: print()" in job.stderr  # library output went to stderr
    assert "chatter: os.write" in job.stderr
    assert (home / "logs" / "worker.log").is_file()


def test_cancel_during_separation(tmp_path: Path, home: Path, started) -> None:
    request = request_for(tmp_path)
    job = started({"SNDS_FAKE_SEGMENTS": "200", "SNDS_FAKE_SEGMENT_MS": "50"})

    job.send(request)
    before = job.events_until(lambda e: e["type"] == "progress" and e["stage"] == "separating")
    job.send({"protocol": 1, "type": "cancel"})
    after, code = job.finish()

    events = before + after
    check_stream(job.raw, events)
    assert events[-1] == {**events[-1], "type": "cancelled", "reason": "requested"}
    assert code == jobs_protocol.EXIT_CANCELLED
    assert not (Path(request["jobDir"]) / "package").exists()
    assert (home / "logs" / "worker.log").is_file()  # the worker log is kept


def test_closing_stdin_cancels(tmp_path: Path, started) -> None:
    request = request_for(tmp_path)
    job = started({"SNDS_FAKE_SEGMENTS": "200", "SNDS_FAKE_SEGMENT_MS": "50"})

    job.send(request)
    before = job.events_until(lambda e: e["type"] == "stage" and e["stage"] == "separating")
    job.close_stdin()
    after, code = job.finish()

    events = before + after
    check_stream(job.raw, events)
    assert events[-1]["type"] == "cancelled"
    assert events[-1]["reason"] == "stdin_closed"
    assert code == jobs_protocol.EXIT_CANCELLED


def test_cancel_line_sent_with_the_request(tmp_path: Path, started) -> None:
    request = request_for(tmp_path)
    job = started()

    job.send(json.dumps(request).encode("ascii") + b'\n{"protocol":1,"type":"cancel"}\n')
    events, code = job.finish()

    check_stream(job.raw, events)
    assert events[0]["type"] == "accepted"
    assert events[-1]["type"] == "cancelled"
    assert code == jobs_protocol.EXIT_CANCELLED


def test_unknown_control_lines_are_ignored(tmp_path: Path, started) -> None:
    request = request_for(tmp_path)
    job = started({"SNDS_FAKE_SEGMENTS": "10", "SNDS_FAKE_SEGMENT_MS": "20"})

    job.send(request)
    job.send({"protocol": 1, "type": "ping"})
    job.send(b"not json at all\n")
    events, code = job.finish()

    check_stream(job.raw, events)
    assert events[-1]["type"] == "result"  # ran to the end, the noise was ignored
    assert "Ignoring unknown control line" in job.stderr
    assert code == jobs_protocol.EXIT_OK


def test_separator_failure_is_reported_with_the_retry_hint(tmp_path: Path, started) -> None:
    request = request_for(tmp_path)
    request["compute"] = {"preference": "auto"}
    job = started({"SNDS_FAKE_DEVICE": "cuda", "SNDS_FAKE_FAIL": "oom"})

    job.send(request)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert events[0]["device"] == "cuda"
    assert events[-1]["code"] == "out_of_memory"
    assert events[-1]["retryable"] is True
    assert events[-1]["suggestedAction"] == "retry_cpu"
    assert code == jobs_protocol.EXIT_JOB_FAILED


@pytest.mark.parametrize(
    ("payload", "code", "failure_code"),
    [
        (b"this is not json\n", 10, "invalid_request"),
        (b"[1, 2]\n", 10, "invalid_request"),
        (b'{"protocol": 1, "type": "prepare"}\n', 10, "invalid_request"),
        (b'{"protocol": 2, "type": "prepare"}\n', 11, "unsupported_protocol_version"),
        (b"\xff\xfe\n", 10, "invalid_request"),
    ],
)
def test_bad_requests(started, payload: bytes, code: int, failure_code: str) -> None:
    job = started()

    job.send(payload)
    events, exit_code = job.finish()

    check_stream(job.raw, events)
    assert len(events) == 1  # a rejected request produces no `accepted`
    assert events[0]["type"] == "failure"
    assert events[0]["code"] == failure_code
    assert exit_code == code
    if code == 11:
        assert events[0]["supportedProtocols"] == [1]


def test_stdin_closed_before_a_request(started) -> None:
    job = started()

    job.close_stdin()
    events, code = job.finish()

    assert [e["code"] for e in events] == ["invalid_request"]
    assert code == jobs_protocol.EXIT_INVALID_REQUEST


def test_the_worker_exits_promptly_after_the_terminal_event(tmp_path: Path, started) -> None:
    request = request_for(tmp_path, seconds=2)
    job = started()

    job.send(request)
    job.events_until(lambda e: e["type"] == "result")
    started_at = time.monotonic()

    assert job.process.wait(timeout=30) == jobs_protocol.EXIT_OK
    assert time.monotonic() - started_at < 30
