"""Job protocol v1 for the long-running `prepare` command (see docs/protocol.md).

Pure and dependency-free: request parsing and validation, event builders, exit codes, and the
NDJSON writer. Nothing here imports torch or touches the file system, so it is cheap to import
before the heavy libraries and trivial to test.
"""

from __future__ import annotations

import json
import ntpath
import os
import re
import sys
import threading
from dataclasses import dataclass
from typing import Any, TextIO

from snds_worker.protocol import PROTOCOL_VERSION

SUPPORTED_PROTOCOLS = (PROTOCOL_VERSION,)

EXIT_OK = 0
EXIT_INVALID_REQUEST = 10
EXIT_UNSUPPORTED_PROTOCOL = 11
EXIT_CANCELLED = 12
EXIT_JOB_FAILED = 13
EXIT_INTERNAL_ERROR = 14
EXIT_KILLED_BY_APP = 99  # assigned by the app when it terminates the job object

STAGES = ("decoding", "separating", "normalizing", "analyzing_bpm", "validating", "finalizing")

COMPUTE_PREFERENCES = ("auto", "cpu")
ARTWORK_ORIGINS = ("embedded", "file", "fallback")

# Stable failure codes. The set may grow within v1; the app treats unknown codes as generic.
FAILURE_CODES = (
    "invalid_request",
    "unsupported_protocol_version",
    "source_unreadable",
    "source_changed",
    "source_unsupported",
    "decode_failed",
    "model_missing",
    "cuda_failed",
    "out_of_memory",
    "separation_failed",
    "normalization_failed",
    "bpm_failed",
    "validation_failed",
    "finalize_failed",
    "cache_conflict",
    "disk_full",
    "internal_error",
)

SUGGESTED_ACTIONS = ("retry_cpu", "retry", "free_disk_space", "fetch_model", "relocate_source")

_JOB_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")
_SHA256 = re.compile(r"[0-9a-f]{64}")


class RequestError(Exception):
    """A request that cannot be accepted. `exit_code` is 10 or 11."""

    def __init__(self, exit_code: int, code: str, message: str, details: str | None = None) -> None:
        super().__init__(message)
        self.exit_code = exit_code
        self.code = code
        self.message = message
        self.details = details

    def to_failure(self) -> dict[str, Any]:
        extra: dict[str, Any] = {}
        if self.code == "unsupported_protocol_version":
            extra["supportedProtocols"] = list(SUPPORTED_PROTOCOLS)
        return failure(self.code, self.message, details=self.details, retryable=False, **extra)


@dataclass(frozen=True)
class SourceInfo:
    path: str
    size_bytes: int
    sha256: str


@dataclass(frozen=True)
class ArtworkInfo:
    path: str
    origin: str


@dataclass(frozen=True)
class PrepareRequest:
    job_id: str
    source: SourceInfo
    job_dir: str
    cache_dir: str
    compute_preference: str
    artwork: ArtworkInfo | None


def _invalid(message: str) -> RequestError:
    return RequestError(EXIT_INVALID_REQUEST, "invalid_request", message)


def _is_plain_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _object(parent: dict[str, Any], key: str, where: str) -> dict[str, Any]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise _invalid(f"{where}{key} must be an object")
    return value


def _string(parent: dict[str, Any], key: str, where: str) -> str:
    value = parent.get(key)
    if not isinstance(value, str) or not value.strip():
        raise _invalid(f"{where}{key} must be a non-empty string")
    return value


def _absolute_path(parent: dict[str, Any], key: str, where: str) -> str:
    value = _string(parent, key, where)
    if not ntpath.isabs(value) or not ntpath.splitdrive(value)[0]:
        raise _invalid(f"{where}{key} must be an absolute Windows path")
    return value


def parse_prepare_request(line: str | bytes) -> PrepareRequest:
    """Validates the first stdin line. Raises RequestError (exit 10 or 11) on any problem."""
    if isinstance(line, bytes):
        try:
            line = line.decode("utf-8")
        except UnicodeDecodeError as error:
            raise _invalid(f"The request is not valid UTF-8: {error}") from error

    if not line.strip():
        raise _invalid("The request is empty")

    try:
        raw = json.loads(line)
    except ValueError as error:
        raise _invalid(f"The request is not valid JSON: {error}") from error

    if not isinstance(raw, dict):
        raise _invalid("The request must be a JSON object")

    version = raw.get("protocol")
    if not _is_plain_int(version):
        raise _invalid("protocol must be an integer")
    if version not in SUPPORTED_PROTOCOLS:
        raise RequestError(
            EXIT_UNSUPPORTED_PROTOCOL,
            "unsupported_protocol_version",
            f"Protocol {version} is not supported; this worker speaks "
            f"{', '.join(str(v) for v in SUPPORTED_PROTOCOLS)}",
        )

    if raw.get("type") != "prepare":
        raise _invalid('type must be "prepare"')

    job_id = raw.get("jobId")
    if not isinstance(job_id, str) or not _JOB_ID.fullmatch(job_id):
        raise _invalid("jobId must be 1-64 characters of letters, digits, '_' or '-'")

    source = _object(raw, "source", "")
    size = source.get("sizeBytes")
    if not _is_plain_int(size) or size < 0:
        raise _invalid("source.sizeBytes must be a non-negative integer")
    sha = source.get("sha256")
    if not isinstance(sha, str) or not _SHA256.fullmatch(sha):
        raise _invalid("source.sha256 must be 64 lowercase hex characters")
    source_info = SourceInfo(_absolute_path(source, "path", "source."), size, sha)

    compute = _object(raw, "compute", "")
    preference = compute.get("preference")
    if preference not in COMPUTE_PREFERENCES:
        raise _invalid(f"compute.preference must be one of {', '.join(COMPUTE_PREFERENCES)}")

    artwork: ArtworkInfo | None = None
    raw_artwork = raw.get("artwork")
    if raw_artwork is not None:
        if not isinstance(raw_artwork, dict):
            raise _invalid("artwork must be an object or null")
        origin = raw_artwork.get("origin")
        if origin not in ARTWORK_ORIGINS:
            raise _invalid(f"artwork.origin must be one of {', '.join(ARTWORK_ORIGINS)}")
        artwork = ArtworkInfo(_absolute_path(raw_artwork, "path", "artwork."), origin)

    return PrepareRequest(
        job_id=job_id,
        source=source_info,
        job_dir=_absolute_path(raw, "jobDir", ""),
        cache_dir=_absolute_path(raw, "cacheDir", ""),
        compute_preference=preference,
        artwork=artwork,
    )


def is_cancel_line(line: str | bytes) -> bool:
    """True for a `{"type":"cancel"}` control line. Anything else is ignored by the caller."""
    try:
        raw = json.loads(line)
    except (ValueError, TypeError):
        return False
    return isinstance(raw, dict) and raw.get("type") == "cancel"


# --- Event builders -----------------------------------------------------------------------


def _event(event_type: str, **fields: Any) -> dict[str, Any]:
    return {
        "protocol": PROTOCOL_VERSION,
        "type": event_type,
        **{key: value for key, value in fields.items() if value is not None},
    }


def accepted(job_id: str, device: str) -> dict[str, Any]:
    return _event("accepted", jobId=job_id, device=device)


def stage(stage_id: str) -> dict[str, Any]:
    if stage_id not in STAGES:
        raise ValueError(f"unknown stage {stage_id!r}")
    return _event("stage", stage=stage_id, index=STAGES.index(stage_id) + 1, count=len(STAGES))


def progress(stage_id: str, fraction: float, eta_seconds: float | None = None) -> dict[str, Any]:
    if stage_id not in STAGES:
        raise ValueError(f"unknown stage {stage_id!r}")
    return _event(
        "progress",
        stage=stage_id,
        fraction=min(1.0, max(0.0, float(fraction))),
        etaSeconds=None if eta_seconds is None else max(0.0, float(eta_seconds)),
    )


def cancelled(reason: str | None = None) -> dict[str, Any]:
    return _event("cancelled", reason=reason)


def failure(
    code: str,
    message: str,
    *,
    details: str | None = None,
    retryable: bool = False,
    suggested_action: str | None = None,
    **extra: Any,
) -> dict[str, Any]:
    return _event(
        "failure",
        code=code,
        message=message,
        details=details,
        retryable=retryable,
        suggestedAction=suggested_action,
        **extra,
    )


def result(cache_dir: str, summary: dict[str, Any]) -> dict[str, Any]:
    return _event("result", cacheDir=cache_dir, summary=summary)


def exit_code_for(event: dict[str, Any]) -> int:
    """The exit code that must accompany a terminal event."""
    kind = event.get("type")
    if kind == "result":
        return EXIT_OK
    if kind == "cancelled":
        return EXIT_CANCELLED
    if kind == "failure":
        code = event.get("code")
        if code == "invalid_request":
            return EXIT_INVALID_REQUEST
        if code == "unsupported_protocol_version":
            return EXIT_UNSUPPORTED_PROTOCOL
        return EXIT_INTERNAL_ERROR if code == "internal_error" else EXIT_JOB_FAILED
    raise ValueError(f"{kind!r} is not a terminal event")


TERMINAL_TYPES = ("result", "failure", "cancelled")


# --- NDJSON writer ------------------------------------------------------------------------


def encode_line(payload: dict[str, Any]) -> str:
    """One compact, pure-ASCII JSON line (non-ASCII becomes \\uXXXX escapes) with its newline."""
    return json.dumps(payload, ensure_ascii=True, separators=(",", ":"), allow_nan=False) + "\n"


class EventWriter:
    """Writes events as NDJSON, one flushed line each, adding a contiguous `seq` from 1.

    After a terminal event nothing more is written. Thread safe: the cancel poller and the job
    thread may both emit.
    """

    def __init__(self, stream: TextIO) -> None:
        self._stream = stream
        self._lock = threading.Lock()
        self._seq = 0
        self._terminal_sent = False
        self._broken = False

    @classmethod
    def open_stdout(cls) -> EventWriter:
        """Takes a private duplicate of the real stdout handle.

        Call before redirecting fd 1 to stderr (protocol.stdout_to_stderr), so stray output
        from libraries can never reach the protocol stream.
        """
        try:
            stream = os.fdopen(
                os.dup(1), "w", encoding="ascii", newline="\n", buffering=1, closefd=True
            )
        except OSError:
            stream = sys.stdout
        return cls(stream)

    @property
    def terminal_sent(self) -> bool:
        return self._terminal_sent

    @property
    def broken(self) -> bool:
        """True once a write failed (the app closed its end of stdout); the job should stop."""
        return self._broken

    def emit(self, event: dict[str, Any]) -> bool:
        """Writes the event; returns False (writing nothing) once a terminal event was sent or
        stdout broke."""
        with self._lock:
            if self._terminal_sent:
                return False
            self._seq += 1
            line = encode_line({**event, "seq": self._seq})
            if event.get("type") in TERMINAL_TYPES:
                self._terminal_sent = True
            if self._broken:
                return False
            try:
                self._stream.write(line)
                self._stream.flush()
            except (OSError, ValueError):
                self._broken = True
                return False
            return True
