"""Versioned NDJSON protocol between the SNDS app and the worker (see docs/protocol.md).

Transport: one JSON object per line on stdout (UTF-8, ASCII-escaped, LF only, flushed per line),
control messages on stdin, human-readable logs on stderr only.
"""

from __future__ import annotations

import datetime as _dt
import enum
import json
import re
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, TextIO

PROTOCOL_VERSION = 1
SUPPORTED_PROTOCOL_VERSIONS: tuple[int, ...] = (1,)

#: Largest accepted request / control line (bytes, excluding the newline).
MAX_LINE_BYTES = 1 << 20

#: Messages after which the worker writes nothing more to stdout and exits.
TERMINAL_TYPES = frozenset({"result", "failed", "cancelled"})


class ExitCode(enum.IntEnum):
    OK = 0
    FAILED = 1
    BAD_REQUEST = 2  # invalid invocation, malformed request, unsupported protocol version
    CUDA_FAILED = 3  # the app may offer an explicit CPU retry in a *new* worker process
    CANCELLED = 4


class ErrorCode(enum.StrEnum):
    # Request / invocation (exit 2). Never retry without changing the request.
    BAD_INVOCATION = "bad-invocation"
    MALFORMED_REQUEST = "malformed-request"
    REQUEST_MISSING = "request-missing"
    REQUEST_TIMEOUT = "request-timeout"
    UNSUPPORTED_PROTOCOL_VERSION = "unsupported-protocol-version"
    INVALID_REQUEST = "invalid-request"
    # Environment (exit 1).
    DEPENDENCY_MISSING = "dependency-missing"
    MODEL_MISSING = "model-missing"
    MODEL_CORRUPT = "model-corrupt"
    MODEL_LOAD_FAILED = "model-load-failed"
    MODEL_DOWNLOAD_FAILED = "model-download-failed"
    DATA_DIR_UNAVAILABLE = "data-dir-unavailable"
    JOB_EXISTS = "job-exists"
    # Source audio (exit 1).
    SOURCE_NOT_FOUND = "source-not-found"
    SOURCE_UNREADABLE = "source-unreadable"
    SOURCE_FINGERPRINT_MISMATCH = "source-fingerprint-mismatch"
    SOURCE_CHANGED = "source-changed"
    UNSUPPORTED_FORMAT = "unsupported-format"
    UNSUPPORTED_CHANNEL_COUNT = "unsupported-channel-count"
    UNSUPPORTED_SAMPLE_RATE = "unsupported-sample-rate"
    SOURCE_EMPTY = "source-empty"
    SOURCE_TOO_LONG = "source-too-long"
    DECODE_FAILED = "decode-failed"
    # Compute (exit 3, retry_on_cpu = true).
    CUDA_UNAVAILABLE = "cuda-unavailable"
    CUDA_FAILED = "cuda-failed"
    # Processing and output (exit 1).
    SEPARATION_FAILED = "separation-failed"
    OUT_OF_MEMORY = "out-of-memory"
    DISK_FULL = "disk-full"
    OUTPUT_WRITE_FAILED = "output-write-failed"
    OUTPUT_VALIDATION_FAILED = "output-validation-failed"
    CACHE_PUBLISH_FAILED = "cache-publish-failed"
    INTERNAL_ERROR = "internal-error"


REQUEST_ERROR_CODES = frozenset(
    {
        ErrorCode.BAD_INVOCATION,
        ErrorCode.MALFORMED_REQUEST,
        ErrorCode.REQUEST_MISSING,
        ErrorCode.REQUEST_TIMEOUT,
        ErrorCode.UNSUPPORTED_PROTOCOL_VERSION,
        ErrorCode.INVALID_REQUEST,
    }
)
CUDA_ERROR_CODES = frozenset({ErrorCode.CUDA_UNAVAILABLE, ErrorCode.CUDA_FAILED})


def exit_code_for(code: ErrorCode) -> ExitCode:
    if code in REQUEST_ERROR_CODES:
        return ExitCode.BAD_REQUEST
    if code in CUDA_ERROR_CODES:
        return ExitCode.CUDA_FAILED
    return ExitCode.FAILED


class WarningCode(enum.StrEnum):
    CUDA_FALLBACK = "cuda-fallback"
    RESIDUAL_HIGH = "residual-high"
    SUBSAMPLE_LAG = "subsample-lag"
    MP3_NO_GAPLESS_INFO = "mp3-no-gapless-info"
    BPM_LOW_CONFIDENCE = "bpm-low-confidence"
    BPM_UNAVAILABLE = "bpm-unavailable"
    REPLACED_CACHE_ENTRY = "replaced-cache-entry"
    CLEANUP_INCOMPLETE = "cleanup-incomplete"
    CONTROL_MESSAGE_IGNORED = "control-message-ignored"
    MODEL_REPLACED = "model-replaced"


class WorkerError(Exception):
    """A failure with a stable error code; becomes one ``failed`` message."""

    def __init__(
        self,
        code: ErrorCode,
        message: str,
        detail: dict[str, Any] | None = None,
        *,
        retry_on_cpu: bool | None = None,
    ) -> None:
        super().__init__(message)
        self.code = ErrorCode(code)
        self.message = message
        self.detail = detail or {}
        self.retry_on_cpu = (
            (self.code in CUDA_ERROR_CODES) if retry_on_cpu is None else retry_on_cpu
        )

    @property
    def exit_code(self) -> ExitCode:
        return exit_code_for(self.code)

    def to_json(self) -> dict[str, Any]:
        return {
            "code": str(self.code),
            "message": self.message,
            "detail": self.detail,
            "retry_on_cpu": self.retry_on_cpu,
        }


class JobCancelled(BaseException):  # noqa: N818 - BaseException so broad `except Exception` never swallows it
    """Raised at the next cancellation point after a cancel request."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


def utc_now() -> str:
    now = _dt.datetime.now(_dt.UTC)
    return now.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def dumps_line(obj: dict[str, Any]) -> str:
    """Compact, ASCII-only, NaN-free JSON (one protocol line without the newline)."""
    return json.dumps(obj, ensure_ascii=True, allow_nan=False, separators=(",", ":"))


class Emitter:
    """Thread-safe NDJSON writer. Every message carries type, protocol_version, seq and ts.

    After a terminal message (result / failed / cancelled) nothing else is written.
    A broken stdout (app gone) is remembered and further writes are dropped silently.
    """

    def __init__(self, stream: TextIO, command: str) -> None:
        self._stream = stream
        self._lock = threading.Lock()
        self._seq = 0
        self.command = command
        self.terminal: dict[str, Any] | None = None
        self.broken = False

    def emit(self, type_: str, **fields: Any) -> dict[str, Any] | None:
        with self._lock:
            if self.terminal is not None:
                return None
            self._seq += 1
            msg: dict[str, Any] = {
                "type": type_,
                "protocol_version": PROTOCOL_VERSION,
                "seq": self._seq,
                "ts": utc_now(),
            }
            msg.update(fields)
            line = dumps_line(msg) + "\n"
            if type_ in TERMINAL_TYPES:
                self.terminal = msg
            if not self.broken:
                try:
                    self._stream.write(line)
                    self._stream.flush()
                except (OSError, ValueError):
                    self.broken = True
            return msg

    # -- helpers for each message type -------------------------------------------------

    def hello(self, **fields: Any) -> None:
        from snds_worker import __version__

        self.emit(
            "hello",
            command=self.command,
            worker_version=__version__,
            protocol_versions=list(SUPPORTED_PROTOCOL_VERSIONS),
            **fields,
        )

    def stage(self, stage: str, index: int, count: int, label: str) -> None:
        self.emit("stage", stage=stage, stage_index=index, stage_count=count, label=label)

    def progress(
        self, stage: str, stage_progress: float, overall: float, detail: dict | None = None
    ) -> None:
        fields: dict[str, Any] = {
            "stage": stage,
            "stage_progress": round(min(1.0, max(0.0, stage_progress)), 4),
            "overall": round(min(1.0, max(0.0, overall)), 4),
        }
        if detail:
            fields["detail"] = detail
        self.emit("progress", **fields)

    def log(self, level: str, message: str, code: str | None = None, **detail: Any) -> None:
        fields: dict[str, Any] = {"level": level, "message": message}
        if code is not None:
            fields["code"] = str(code)
        if detail:
            fields["detail"] = detail
        self.emit("log", **fields)

    def warning(self, code: WarningCode, message: str, **detail: Any) -> None:
        self.log("warning", message, code=code, **detail)

    def device(self, **fields: Any) -> None:
        self.emit("device", **fields)

    def cancel_ack(self, reason: str, stage: str | None) -> None:
        self.emit("cancel_ack", reason=reason, stage=stage)

    def cancelled(self, reason: str, stage: str | None, **fields: Any) -> None:
        self.emit(
            "cancelled",
            command=self.command,
            reason=reason,
            stage=stage,
            exit_code=int(ExitCode.CANCELLED),
            **fields,
        )

    def failed(self, error: WorkerError, stage: str | None) -> None:
        self.emit(
            "failed",
            command=self.command,
            stage=stage,
            error=error.to_json(),
            exit_code=int(error.exit_code),
        )

    def result(self, **payload: Any) -> None:
        self.emit("result", command=self.command, exit_code=int(ExitCode.OK), **payload)


# --- job request ----------------------------------------------------------------------

JOB_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
DEVICES = ("auto", "cpu", "cuda")


@dataclass(frozen=True)
class SourceSpec:
    path: Path
    sha256: str
    size_bytes: int


@dataclass(frozen=True)
class JobRequest:
    job_id: str
    source: SourceSpec
    device: str = "auto"
    data_root: Path | None = None


def _invalid(message: str, **detail: Any) -> WorkerError:
    return WorkerError(ErrorCode.INVALID_REQUEST, message, detail or None)


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def check_protocol_version(value: Any) -> None:
    if not _is_int(value) or value not in SUPPORTED_PROTOCOL_VERSIONS:
        raise WorkerError(
            ErrorCode.UNSUPPORTED_PROTOCOL_VERSION,
            f"Unsupported protocol version {value!r}; this worker speaks "
            f"{list(SUPPORTED_PROTOCOL_VERSIONS)}.",
            {"requested": value, "supported": list(SUPPORTED_PROTOCOL_VERSIONS)},
        )


def _check_keys(obj: dict, allowed: set[str], required: set[str], where: str) -> None:
    unknown = sorted(set(obj) - allowed)
    if unknown:
        raise _invalid(f"Unknown field(s) in {where}: {', '.join(unknown)}", fields=unknown)
    missing = sorted(required - set(obj))
    if missing:
        raise _invalid(f"Missing field(s) in {where}: {', '.join(missing)}", fields=missing)


def _absolute_path(value: Any, where: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise _invalid(f"{where} must be a non-empty string")
    path = Path(value)
    if not path.is_absolute():
        raise _invalid(f"{where} must be an absolute path", value=value)
    return path


def parse_job_request(raw: bytes | str) -> JobRequest:
    """Parse and validate the single ``separate`` request line. Raises WorkerError."""
    if isinstance(raw, bytes):
        if len(raw) > MAX_LINE_BYTES:
            raise WorkerError(
                ErrorCode.MALFORMED_REQUEST,
                f"Request line exceeds {MAX_LINE_BYTES} bytes",
                {"bytes": len(raw)},
            )
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            raise WorkerError(
                ErrorCode.MALFORMED_REQUEST, "Request is not valid UTF-8", {"error": str(exc)}
            ) from exc
    else:
        text = raw
    text = text.strip()
    try:
        obj = json.loads(text)
    except json.JSONDecodeError as exc:
        raise WorkerError(
            ErrorCode.MALFORMED_REQUEST, f"Request is not valid JSON: {exc.msg}", {"pos": exc.pos}
        ) from exc
    if not isinstance(obj, dict):
        raise WorkerError(ErrorCode.MALFORMED_REQUEST, "Request must be a JSON object")

    # The version is checked first: other versions may use a different shape.
    check_protocol_version(obj.get("protocol_version"))

    _check_keys(
        obj,
        allowed={"type", "protocol_version", "job_id", "source", "options", "data_root"},
        required={"type", "protocol_version", "job_id", "source"},
        where="request",
    )
    if obj["type"] != "separate":
        raise _invalid(f"Unexpected request type {obj['type']!r}; expected 'separate'")

    job_id = obj["job_id"]
    if not isinstance(job_id, str) or not JOB_ID_RE.match(job_id):
        raise _invalid("job_id must match [A-Za-z0-9][A-Za-z0-9_-]{0,63}", value=job_id)

    source = obj["source"]
    if not isinstance(source, dict):
        raise _invalid("source must be an object")
    _check_keys(
        source,
        allowed={"path", "sha256", "size_bytes"},
        required={"path", "sha256", "size_bytes"},
        where="source",
    )
    path = _absolute_path(source["path"], "source.path")
    sha = source["sha256"]
    if not isinstance(sha, str) or not SHA256_RE.match(sha):
        raise _invalid("source.sha256 must be 64 lowercase hex characters")
    size = source["size_bytes"]
    if not _is_int(size) or size < 0:
        raise _invalid("source.size_bytes must be a non-negative integer")

    options = obj.get("options", {})
    if not isinstance(options, dict):
        raise _invalid("options must be an object")
    _check_keys(options, allowed={"device"}, required=set(), where="options")
    device = options.get("device", "auto")
    if device not in DEVICES:
        raise _invalid(f"options.device must be one of {list(DEVICES)}", value=device)

    data_root = None
    if "data_root" in obj and obj["data_root"] is not None:
        data_root = _absolute_path(obj["data_root"], "data_root")

    return JobRequest(
        job_id=job_id,
        source=SourceSpec(path=path, sha256=sha, size_bytes=size),
        device=device,
        data_root=data_root,
    )
