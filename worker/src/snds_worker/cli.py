"""Command line: ``python -u -X utf8 -m snds_worker <command>`` (see docs/protocol.md).

Commands: ``version``, ``readiness``, ``fetch-model``, ``separate``. Every command writes
NDJSON protocol messages to stdout (first ``hello``, last exactly one terminal message) and
logs to stderr.
"""

from __future__ import annotations

import argparse
import contextlib
import logging
import os
import platform
import sys
from pathlib import Path
from typing import TextIO

from snds_worker import __version__, faults
from snds_worker.cache import PIPELINE_ID, TEST_PIPELINE_ID
from snds_worker.control import EOF, TOO_LONG, CancelToken, StdinChannel
from snds_worker.logsetup import configure_logging
from snds_worker.protocol import (
    MAX_LINE_BYTES,
    SUPPORTED_PROTOCOL_VERSIONS,
    Emitter,
    ErrorCode,
    ExitCode,
    JobCancelled,
    WarningCode,
    WorkerError,
    check_protocol_version,
    parse_job_request,
)

log = logging.getLogger("snds_worker")

DEFAULT_REQUEST_TIMEOUT = 30.0


class _UsageError(Exception):
    pass


class _Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:  # type: ignore[override]
        raise _UsageError(message)

    def exit(self, status: int = 0, message: str | None = None) -> None:  # type: ignore[override]
        if message:
            sys.stderr.write(message)
        raise _UsageError(message or "help requested")


def build_parser() -> argparse.ArgumentParser:
    p = _Parser(prog="python -m snds_worker", description="SNDS preparation worker")
    sub = p.add_subparsers(dest="command", required=True)

    def common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument(
            "--protocol-version",
            type=int,
            default=None,
            help="Protocol version the caller speaks; unsupported versions are rejected.",
        )

    v = sub.add_parser("version", help="Report worker and protocol versions.")
    common(v)

    r = sub.add_parser("readiness", help="Check the environment (packages, model, CPU, CUDA).")
    common(r)
    r.add_argument("--data-root", type=Path, default=None)
    r.add_argument("--force-cpu", action="store_true", help="Honour the app's Force CPU setting.")
    r.add_argument("--full-verify", action="store_true", help="Always re-hash the model file.")
    r.add_argument("--probe-timeout", type=float, default=30.0)

    f = sub.add_parser("fetch-model", help="Download and verify the pinned htdemucs checkpoint.")
    common(f)
    f.add_argument("--data-root", type=Path, default=None)

    s = sub.add_parser(
        "separate", help="Run one job. Reads the JSON request as the first stdin line."
    )
    s.add_argument("--request-timeout", type=float, default=DEFAULT_REQUEST_TIMEOUT)
    return p


def _pipeline_id() -> str:
    return TEST_PIPELINE_ID if faults.fake_separator_enabled() else PIPELINE_ID


def cmd_version(args: argparse.Namespace, em: Emitter) -> int:
    em.result(
        worker_version=__version__,
        protocol_versions=list(SUPPORTED_PROTOCOL_VERSIONS),
        pipeline_id=_pipeline_id(),
        python=platform.python_version(),
    )
    return int(ExitCode.OK)


def cmd_readiness(args: argparse.Namespace, em: Emitter) -> int:
    from snds_worker.readiness import Readiness

    r = Readiness(args.data_root, args.force_cpu, args.full_verify, args.probe_timeout)
    report = r.run()
    em.result(readiness=report)
    if r.probe_timed_out:
        # A hung CUDA call cannot be interrupted; do not wait for it at interpreter shutdown.
        logging.shutdown()
        sys.stderr.flush()
        os._exit(int(ExitCode.OK))
    return int(ExitCode.OK)


def cmd_fetch_model(args: argparse.Namespace, em: Emitter) -> int:
    from snds_worker.fetch import fetch_model
    from snds_worker.paths import DataDirs

    payload = fetch_model(em, DataDirs.resolve(args.data_root))
    em.result(**payload)
    return int(ExitCode.OK)


def cmd_separate(args: argparse.Namespace, em: Emitter) -> int:
    from snds_worker.job import SeparateJob

    token = CancelToken()
    current: dict[str, SeparateJob] = {}

    def on_cancel(reason: str) -> None:
        job = current.get("job")
        log.info("cancel requested (%s)", reason)
        em.cancel_ack(reason, job.stages.current if job else None)

    def on_ignored(text: str) -> None:
        em.warning(
            WarningCode.CONTROL_MESSAGE_IGNORED, "Ignored an unknown control line.", line=text
        )

    channel = StdinChannel(token, on_cancel, on_ignored)
    channel.start()
    raw = channel.wait_request(args.request_timeout)
    if raw is None:
        raise WorkerError(
            ErrorCode.REQUEST_TIMEOUT,
            f"No job request arrived on stdin within {args.request_timeout:g} s.",
        )
    if raw is EOF:
        raise WorkerError(ErrorCode.REQUEST_MISSING, "stdin closed before a job request arrived.")
    if raw is TOO_LONG:
        raise WorkerError(
            ErrorCode.MALFORMED_REQUEST, f"The request line exceeds {MAX_LINE_BYTES} bytes."
        )
    request = parse_job_request(raw)  # type: ignore[arg-type]
    job = SeparateJob(request, em, token)
    current["job"] = job
    return job.run()


COMMANDS = {
    "version": cmd_version,
    "readiness": cmd_readiness,
    "fetch-model": cmd_fetch_model,
    "separate": cmd_separate,
}


def run(argv: list[str] | None, proto: TextIO) -> int:
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except _UsageError as exc:
        em = Emitter(proto, "")
        em.hello(pid=os.getpid())
        em.failed(
            WorkerError(
                ErrorCode.BAD_INVOCATION,
                f"Invalid command line: {exc}. Commands: {', '.join(COMMANDS)}.",
            ),
            None,
        )
        return int(ExitCode.BAD_REQUEST)
    em = Emitter(proto, args.command)
    em.hello(pid=os.getpid(), python=platform.python_version(), pipeline_id=_pipeline_id())
    try:
        if getattr(args, "protocol_version", None) is not None:
            check_protocol_version(args.protocol_version)
        return COMMANDS[args.command](args, em)
    except WorkerError as exc:
        log.error("%s: %s", exc.code, exc.message)
        em.failed(exc, None)
        return int(exc.exit_code)
    except (KeyboardInterrupt, JobCancelled) as exc:
        reason = exc.reason if isinstance(exc, JobCancelled) else "interrupt"
        em.cancelled(reason, None)
        return int(ExitCode.CANCELLED)
    except Exception as exc:
        log.exception("unexpected error")
        em.failed(
            WorkerError(
                ErrorCode.INTERNAL_ERROR,
                f"Unexpected worker error: {type(exc).__name__}: {exc}",
                {"exception": type(exc).__name__},
            ),
            None,
        )
        return int(ExitCode.FAILED)


def open_protocol_stream() -> TextIO:
    """Reserve the real stdout for protocol lines and point fd 1 (and ``sys.stdout``) at
    stderr, so stray prints from libraries can never corrupt the NDJSON stream."""
    sys.stdout.flush()
    fd = os.dup(1)
    os.dup2(2, 1)
    if sys.platform == "win32":
        import msvcrt

        msvcrt.setmode(fd, os.O_BINARY)
    stream = open(fd, "w", encoding="utf-8", newline="\n", buffering=1)  # noqa: SIM115
    with contextlib.suppress(AttributeError, ValueError):
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace", line_buffering=True)  # type: ignore[union-attr]
    sys.stdout = sys.stderr
    return stream


def main(argv: list[str] | None = None) -> int:
    proto = open_protocol_stream()
    configure_logging()
    code = run(argv, proto)
    logging.shutdown()
    with contextlib.suppress(OSError):
        proto.flush()
    return code
