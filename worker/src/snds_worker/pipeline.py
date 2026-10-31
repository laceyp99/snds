"""The `prepare` job: decode, separate, normalize, analyze BPM, validate, finalize.

`run_prepare` owns the protocol conversation after the request was accepted: stage and progress
events, and exactly one terminal event with the matching exit code. Everything it needs from the
outside (the separator, the device choice, BPM analysis, finalization) is passed in, so tests
drive the real pipeline with a fake separator.
"""

from __future__ import annotations

import logging
import shutil
import time
import traceback
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

from snds_worker import bpm as bpm_module
from snds_worker import decode, diagnostics, jobs_protocol, normalize, separation, validate
from snds_worker.cancel import CancelToken
from snds_worker.decode import DecodedSource
from snds_worker.errors import JobCancelled, JobFailure
from snds_worker.finalize import finalize_package
from snds_worker.jobs_protocol import EventWriter, PrepareRequest
from snds_worker.normalize import WrittenStem
from snds_worker.separation import STEMS, SeparatorFactory
from snds_worker.validate import ValidationReport

log = logging.getLogger(__name__)

PACKAGE_DIR = "package"  # inside jobDir; renamed to cacheDir when finalized

_PROGRESS_STEP = 0.01
_PROGRESS_SECONDS = 0.25
_ETA_MIN_FRACTION = 0.05
_ETA_MIN_SECONDS = 5.0


@dataclass
class PreparedJob:
    """Everything the later stages need once the stems are written and validated."""

    request: PrepareRequest
    device: str
    source: DecodedSource
    package_dir: Path
    stems: dict[str, WrittenStem]
    validation: ValidationReport
    bpm: bpm_module.BpmOutcome | None = None
    timings: dict[str, float] = field(default_factory=dict)
    started_at: float = 0.0


BpmStage = Callable[
    [DecodedSource, str, CancelToken, Callable[[float], None]], bpm_module.BpmOutcome
]
FinalizeStage = Callable[[PreparedJob, CancelToken], dict[str, Any]]


def bpm_not_available(
    _source: DecodedSource, _device: str, _token: CancelToken, _progress: Callable[[float], None]
) -> bpm_module.BpmOutcome:
    return bpm_module.failed("BPM analysis is not available in this build")


class Reporter:
    """Throttled `progress` events for one stage."""

    def __init__(
        self, writer: EventWriter, token: CancelToken, stage_id: str, *, eta: bool = False
    ) -> None:
        self._writer = writer
        self._token = token
        self._stage = stage_id
        self._eta = eta
        self._started = time.monotonic()
        self._last_fraction = -1.0
        self._last_time = 0.0

    def __call__(self, fraction: float) -> None:
        fraction = min(1.0, max(0.0, fraction))
        now = time.monotonic()
        if fraction < 1.0 and (
            fraction - self._last_fraction < _PROGRESS_STEP
            or now - self._last_time < _PROGRESS_SECONDS
        ):
            return

        eta = None
        elapsed = now - self._started
        if self._eta and fraction >= _ETA_MIN_FRACTION and elapsed >= _ETA_MIN_SECONDS:
            eta = elapsed * (1.0 - fraction) / fraction

        self._last_fraction = fraction
        self._last_time = now
        self._writer.emit(jobs_protocol.progress(self._stage, fraction, eta))
        if self._writer.broken:
            self._token.trigger("stdout_closed")

    def done(self) -> None:
        if self._last_fraction < 1.0:
            self(1.0)


def scaled(reporter: Callable[[float], None], start: float, end: float) -> Callable[[float], None]:
    return lambda fraction: reporter(start + (end - start) * min(1.0, max(0.0, fraction)))


def remove_incomplete(job_dir: Path) -> None:
    """Deletes the audio written so far (the package folder and temporary files).

    Logs live in SNDS_HOME\\logs, and the app's own files in jobDir (such as artwork) stay.
    """
    shutil.rmtree(job_dir / PACKAGE_DIR, ignore_errors=True)
    for leftover in job_dir.glob("*.part"):
        leftover.unlink(missing_ok=True)


def run_prepare(
    request: PrepareRequest,
    writer: EventWriter,
    token: CancelToken,
    *,
    separator_factory: SeparatorFactory,
    model_dir: Path,
    resolve_device: Callable[[str], str] = separation.resolve_device,
    analyze_bpm: BpmStage = bpm_not_available,
    finalize: FinalizeStage = finalize_package,
) -> int:
    """Runs the job to its terminal event and returns the matching exit code."""
    job_dir = Path(request.job_dir)
    device = "cpu"
    try:
        device = resolve_device(request.compute_preference)
        writer.emit(jobs_protocol.accepted(request.job_id, device))
        log.info("Job %s accepted on %s", request.job_id, device)
        terminal = _run_stages(
            request,
            writer,
            token,
            device,
            separator_factory,
            model_dir,
            analyze_bpm,
            finalize,
        )
    except JobCancelled as cancel:
        remove_incomplete(job_dir)
        log.info("Job %s cancelled (%s)", request.job_id, cancel.reason)
        terminal = jobs_protocol.cancelled(cancel.reason)
    except JobFailure as error:
        remove_incomplete(job_dir)
        log.error("Job %s failed: [%s] %s", request.job_id, error.code, error.message)
        if error.details:
            log.error("%s", error.details)
        terminal = jobs_protocol.failure(
            error.code,
            error.message,
            details=error.details,
            retryable=error.retryable,
            suggested_action=error.suggested_action,
        )
    except Exception as error:
        remove_incomplete(job_dir)
        log.exception("Job %s failed unexpectedly", request.job_id)
        known = separation.classify_error(error, device)  # memory and disk exhaustion
        if known.code != "separation_failed":
            terminal = jobs_protocol.failure(
                known.code,
                known.message,
                details=known.details,
                retryable=known.retryable,
                suggested_action=known.suggested_action,
            )
        else:
            terminal = jobs_protocol.failure(
                "internal_error",
                f"Unexpected error: {type(error).__name__}: {error}",
                details="".join(traceback.format_exception(error))[-4000:],
            )

    writer.emit(terminal)
    return jobs_protocol.exit_code_for(terminal)


def _run_stages(
    request: PrepareRequest,
    writer: EventWriter,
    token: CancelToken,
    device: str,
    separator_factory: SeparatorFactory,
    model_dir: Path,
    analyze_bpm: BpmStage,
    finalize: FinalizeStage,
) -> dict[str, Any]:
    started = time.monotonic()
    timings: dict[str, float] = {}
    stage_started = started

    def begin(stage_id: str, *, eta: bool = False) -> Reporter:
        nonlocal stage_started
        token.raise_if_cancelled()
        now = time.monotonic()
        stage_started = now
        writer.emit(jobs_protocol.stage(stage_id))
        return Reporter(writer, token, stage_id, eta=eta)

    def end(stage_id: str, reporter: Reporter) -> None:
        reporter.done()
        timings[stage_id] = time.monotonic() - stage_started
        log.info(
            "Stage %s took %.1f s (%s)",
            stage_id,
            timings[stage_id],
            diagnostics.describe_peak_memory(),
        )

    # 1. decoding
    reporter = begin("decoding")
    source_path = Path(request.source.path)
    decode.verify_source(
        source_path,
        request.source.size_bytes,
        request.source.sha256,
        token,
        scaled(reporter, 0.0, decode.HASH_SHARE),
    )
    source = decode.decode_source(source_path, token, scaled(reporter, decode.HASH_SHARE, 1.0))
    end("decoding", reporter)

    package_dir = Path(request.job_dir) / PACKAGE_DIR
    stems: dict[str, WrittenStem] = {}
    try:
        # 2. separating
        reporter = begin("separating", eta=True)
        shutil.rmtree(package_dir, ignore_errors=True)
        package_dir.mkdir(parents=True)
        separator = separator_factory(device, model_dir)
        wav = normalize.to_model_input(source)
        token.raise_if_cancelled()
        separated = separator.separate(wav, reporter, token)
        del wav
        if tuple(separated.shape[:2]) != (len(STEMS), 2):
            raise JobFailure(
                "separation_failed",
                "The separator returned an unexpected output shape.",
                details=f"shape {tuple(separated.shape)}",
            )
        end("separating", reporter)

        # 3. normalizing
        reporter = begin("normalizing")
        for index, name in enumerate(STEMS):
            token.raise_if_cancelled()
            stems[name] = normalize.write_stem(name, separated[index], source, package_dir)
            reporter((index + 1) / len(STEMS))
        del separated
        end("normalizing", reporter)
    except (JobFailure, JobCancelled):
        raise
    except Exception as error:
        raise separation.classify_error(error, device) from error

    # 4. analyzing_bpm
    reporter = begin("analyzing_bpm")
    outcome = analyze_bpm(source, device, token, reporter)
    token.raise_if_cancelled()
    end("analyzing_bpm", reporter)

    # 5. validating
    reporter = begin("validating")
    report = validate.validate_stems(package_dir, source, token, reporter)
    end("validating", reporter)

    source.audio = np.empty((0, source.channels), dtype=np.float32)  # no longer needed

    # 6. finalizing
    reporter = begin("finalizing")
    job = PreparedJob(
        request=request,
        device=device,
        source=source,
        package_dir=package_dir,
        stems=stems,
        validation=report,
        bpm=outcome,
        timings=timings,
        started_at=started,
    )
    summary = finalize(job, token)
    end("finalizing", reporter)
    log.info("Job %s finished in %.1f s", request.job_id, time.monotonic() - started)
    return jobs_protocol.result(request.cache_dir, summary)
