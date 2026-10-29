from __future__ import annotations

import io
import json
import os
import re
import threading
from pathlib import Path

import pytest

from snds_worker import control
from snds_worker.protocol import (
    MAX_LINE_BYTES,
    Emitter,
    ErrorCode,
    ExitCode,
    WarningCode,
    WorkerError,
    exit_code_for,
    parse_job_request,
)

DOCS = Path(__file__).resolve().parents[2] / "docs" / "protocol.md"
SHA = "a" * 64


def _request(**over) -> dict:
    req = {
        "type": "separate",
        "protocol_version": 1,
        "job_id": "job-1",
        "source": {"path": "C:\\music\\song.wav", "sha256": SHA, "size_bytes": 1234},
    }
    req.update(over)
    return req


def _parse(obj) -> object:
    return parse_job_request(json.dumps(obj).encode())


# --- emitter ---------------------------------------------------------------------------


def test_emitter_envelope_ascii_lf_and_sequence() -> None:
    buf = io.StringIO()
    em = Emitter(buf, "separate")
    em.hello(pid=1)
    em.stage("validating-input", 0, 7, "Checking")
    em.progress("validating-input", 0.5, 0.1, {"bytes_hashed": 10})
    em.log("info", "caf\u00e9 \u266b")
    em.warning(WarningCode.CUDA_FALLBACK, "fallback")
    em.device(requested="auto", selected="cpu")
    em.cancel_ack("cancel-message", "separating")
    em.result(cache_key="k")
    text = buf.getvalue()
    assert "\r" not in text
    assert text.isascii()
    lines = text.split("\n")
    assert lines[-1] == ""
    msgs = [json.loads(line) for line in lines[:-1]]
    assert [m["seq"] for m in msgs] == list(range(1, 9))
    for m in msgs:
        assert m["protocol_version"] == 1
        assert re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$", m["ts"])
    assert msgs[3]["message"] == "caf\u00e9 \u266b"
    assert msgs[4]["code"] == "cuda-fallback" and msgs[4]["level"] == "warning"
    assert msgs[-1]["type"] == "result" and msgs[-1]["exit_code"] == 0


def test_nothing_is_emitted_after_a_terminal_message() -> None:
    buf = io.StringIO()
    em = Emitter(buf, "separate")
    em.failed(WorkerError(ErrorCode.CUDA_FAILED, "boom"), "separating")
    em.progress("separating", 1.0, 1.0)
    em.result()
    msgs = [json.loads(line) for line in buf.getvalue().splitlines()]
    assert len(msgs) == 1
    err = msgs[0]["error"]
    assert (
        msgs[0]["exit_code"] == 3 and err["retry_on_cpu"] is True and err["code"] == "cuda-failed"
    )


def test_cancelled_message_shape() -> None:
    buf = io.StringIO()
    Emitter(buf, "separate").cancelled("stdin-eof", "separating", job_dir="x")
    msg = json.loads(buf.getvalue())
    assert msg["type"] == "cancelled" and msg["exit_code"] == 4 and msg["reason"] == "stdin-eof"


def test_emitter_rejects_nan() -> None:
    with pytest.raises(ValueError):
        Emitter(io.StringIO(), "x").emit("log", value=float("nan"))


def test_emitter_survives_a_broken_stdout() -> None:
    class Broken(io.StringIO):
        def write(self, s: str) -> int:
            raise BrokenPipeError

    em = Emitter(Broken(), "x")
    em.hello()
    assert em.broken


def test_emitter_is_thread_safe() -> None:
    buf = io.StringIO()
    em = Emitter(buf, "x")
    threads = [
        threading.Thread(target=lambda: [em.log("info", "m") for _ in range(200)]) for _ in range(4)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    seqs = [json.loads(line)["seq"] for line in buf.getvalue().splitlines()]
    assert sorted(seqs) == list(range(1, 801))


# --- codes -----------------------------------------------------------------------------


def test_exit_code_mapping() -> None:
    assert exit_code_for(ErrorCode.MALFORMED_REQUEST) == ExitCode.BAD_REQUEST
    assert exit_code_for(ErrorCode.UNSUPPORTED_PROTOCOL_VERSION) == 2
    assert exit_code_for(ErrorCode.CUDA_FAILED) == 3
    assert exit_code_for(ErrorCode.CUDA_UNAVAILABLE) == 3
    assert exit_code_for(ErrorCode.OUTPUT_VALIDATION_FAILED) == 1
    assert [int(c) for c in ExitCode] == [0, 1, 2, 3, 4]


def test_codes_are_stable_kebab_case_and_documented() -> None:
    docs = DOCS.read_text(encoding="utf-8")
    for code in [*ErrorCode, *WarningCode]:
        assert re.match(r"^[a-z0-9]+(-[a-z0-9]+)*$", code.value)
        assert f"`{code.value}`" in docs, f"{code.value} is not documented in docs/protocol.md"
    assert len({c.value for c in ErrorCode}) == len(ErrorCode)


# --- request parsing ---------------------------------------------------------------------


def test_minimal_request() -> None:
    req = _parse(_request())
    assert req.job_id == "job-1"
    assert req.device == "auto"
    assert req.data_root is None
    assert req.source.size_bytes == 1234


def test_full_request_with_bom_and_crlf() -> None:
    raw = "\ufeff" + json.dumps(_request(options={"device": "cpu"}, data_root="D:\\snds")) + "\r"
    req = parse_job_request(raw.encode("utf-8"))
    assert req.device == "cpu"
    assert str(req.data_root) == "D:\\snds"


@pytest.mark.parametrize("version", [0, 2, 99, "1", True, None, 1.0])
def test_unknown_protocol_versions_are_rejected(version) -> None:
    obj = _request(protocol_version=version)
    with pytest.raises(WorkerError) as ei:
        _parse(obj)
    assert ei.value.code == ErrorCode.UNSUPPORTED_PROTOCOL_VERSION
    assert ei.value.exit_code == 2


def test_missing_protocol_version_is_rejected() -> None:
    obj = _request()
    del obj["protocol_version"]
    with pytest.raises(WorkerError) as ei:
        _parse(obj)
    assert ei.value.code == ErrorCode.UNSUPPORTED_PROTOCOL_VERSION


@pytest.mark.parametrize(
    "raw",
    [b"not json", b"[1, 2]", b'"text"', b"\xff\xfe{}", b"{", b""],
)
def test_malformed_requests(raw: bytes) -> None:
    with pytest.raises(WorkerError) as ei:
        parse_job_request(raw)
    assert ei.value.code == ErrorCode.MALFORMED_REQUEST
    assert ei.value.exit_code == 2


def test_oversized_request() -> None:
    with pytest.raises(WorkerError) as ei:
        parse_job_request(b" " * (MAX_LINE_BYTES + 1))
    assert ei.value.code == ErrorCode.MALFORMED_REQUEST


@pytest.mark.parametrize(
    "mutate",
    [
        lambda r: r.update(extra=1),
        lambda r: r.update(type="readiness"),
        lambda r: r.update(job_id="../escape"),
        lambda r: r.update(job_id=""),
        lambda r: r.update(job_id="x" * 65),
        lambda r: r.update(source="C:\\a.wav"),
        lambda r: r["source"].update(path="relative\\a.wav"),
        lambda r: r["source"].update(path=""),
        lambda r: r["source"].update(sha256="A" * 64),
        lambda r: r["source"].update(sha256="abc"),
        lambda r: r["source"].update(size_bytes=-1),
        lambda r: r["source"].update(size_bytes=True),
        lambda r: r["source"].update(size_bytes="12"),
        lambda r: r["source"].update(extra=1),
        lambda r: r["source"].pop("sha256"),
        lambda r: r.pop("job_id"),
        lambda r: r.update(options={"device": "gpu"}),
        lambda r: r.update(options={"force_cpu": True}),
        lambda r: r.update(options=[]),
        lambda r: r.update(data_root="relative"),
    ],
)
def test_invalid_requests(mutate) -> None:
    obj = _request()
    mutate(obj)
    with pytest.raises(WorkerError) as ei:
        _parse(obj)
    assert ei.value.code == ErrorCode.INVALID_REQUEST
    assert ei.value.exit_code == 2


# --- stdin control channel ---------------------------------------------------------------


class _Channel:
    def __init__(self) -> None:
        self.r, self.w = os.pipe()
        self.token = control.CancelToken()
        self.cancels: list[str] = []
        self.ignored: list[str] = []
        self.ch = control.StdinChannel(
            self.token, self.cancels.append, self.ignored.append, fd=self.r
        )
        self.ch.start()

    def write(self, data: bytes) -> None:
        os.write(self.w, data)

    def close(self) -> None:
        os.close(self.w)

    def wait_done(self) -> None:
        self.ch._thread.join(5)
        os.close(self.r)


def test_channel_request_then_cancel_message() -> None:
    c = _Channel()
    c.write(b'{"type":"separate"}\r\n')
    assert c.ch.wait_request(5) == b'{"type":"separate"}'
    c.write(b'{"type":"ping"}\nnot json\n\n')
    c.write(b'{"type":"cancel","protocol_version":1}\n')
    c.close()
    c.wait_done()
    assert c.cancels == ["cancel-message"]  # EOF afterwards does not cancel twice
    assert c.token.requested and c.token.reason == "cancel-message"
    assert c.ignored == ['{"type":"ping"}', "not json"]
    with pytest.raises(BaseException) as ei:
        c.token.check()
    assert type(ei.value).__name__ == "JobCancelled"


def test_channel_eof_after_request_cancels() -> None:
    c = _Channel()
    c.write(b"{}\n")
    assert c.ch.wait_request(5) == b"{}"
    c.close()
    c.wait_done()
    assert c.cancels == ["stdin-eof"]


def test_channel_eof_before_request() -> None:
    c = _Channel()
    c.close()
    assert c.ch.wait_request(5) is control.EOF
    c.wait_done()
    assert c.cancels == []


def test_channel_request_without_newline_then_eof() -> None:
    c = _Channel()
    c.write(b'{"a":1}')
    c.close()
    assert c.ch.wait_request(5) == b'{"a":1}'
    c.wait_done()
    assert c.cancels == ["stdin-eof"]


def test_channel_too_long_request_and_timeout() -> None:
    c = _Channel()
    assert c.ch.wait_request(0.1) is None
    c.write(b"x" * (MAX_LINE_BYTES + 10))
    assert c.ch.wait_request(5) is control.TOO_LONG
    c.write(b"yyy\n")
    c.write(b'{"type":"cancel"}\n')
    c.close()
    c.wait_done()
    assert c.cancels == ["cancel-message"]
