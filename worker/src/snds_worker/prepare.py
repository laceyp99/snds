"""The `prepare` command: request in, NDJSON events out (see docs/protocol.md section 3)."""

from __future__ import annotations

import logging
import os
import traceback
from collections.abc import Mapping

from snds_worker import (
    bpm,
    fake_separator,
    jobs_protocol,
    model,
    pipeline,
    protocol,
    separation,
)
from snds_worker.cancel import CancelToken, StdinControl, read_request_line
from snds_worker.jobs_protocol import EventWriter, RequestError
from snds_worker.logs import configure_logging
from snds_worker.paths import HomeUnavailableError, data_paths

log = logging.getLogger(__name__)


def run(environ: Mapping[str, str] | None = None) -> int:
    """Runs one job and returns the process exit code (section 3.4)."""
    env = os.environ if environ is None else environ

    # The private duplicate of stdout must exist before fd 1 is redirected to stderr.
    writer = EventWriter.open_stdout()

    try:
        paths = data_paths(env)
    except HomeUnavailableError:
        paths = None
    if paths is not None:
        try:
            configure_logging(paths.logs)
        except OSError:
            log.warning("The worker log file cannot be opened; logging to stderr only.")

    with protocol.stdout_to_stderr():
        raw, pending = read_request_line()
        try:
            if raw is None:
                raise RequestError(
                    jobs_protocol.EXIT_INVALID_REQUEST,
                    "invalid_request",
                    "stdin closed before a request was received",
                )
            request = jobs_protocol.parse_prepare_request(raw)
        except RequestError as error:
            log.error("Rejected the request: [%s] %s", error.code, error.message)
            writer.emit(error.to_failure())
            return error.exit_code

        token = CancelToken()
        StdinControl(token, pending).start()

        if fake_separator.enabled(env):
            factory = fake_separator.make_fake_separator

            def resolve(preference: str) -> str:
                return "cpu" if preference == "cpu" else env.get("SNDS_FAKE_DEVICE", "cpu")

        else:
            factory = separation.make_demucs_separator
            resolve = separation.resolve_device

        if paths is None:
            writer.emit(
                jobs_protocol.failure(
                    "internal_error", "Neither SNDS_HOME nor LOCALAPPDATA is set."
                )
            )
            return jobs_protocol.EXIT_INTERNAL_ERROR

        try:
            return pipeline.run_prepare(
                request,
                writer,
                token,
                separator_factory=factory,
                model_dir=paths.models / "htdemucs",
                analyze_bpm=bpm.make_analyzer(model.beat_model_dir(paths)),
                resolve_device=resolve,
            )
        except Exception as error:  # run_prepare reports its own errors; this is the last resort
            log.exception("The prepare job crashed")
            writer.emit(
                jobs_protocol.failure(
                    "internal_error",
                    f"Unexpected error: {type(error).__name__}: {error}",
                    details="".join(traceback.format_exception(error))[-4000:],
                )
            )
            return jobs_protocol.EXIT_INTERNAL_ERROR
