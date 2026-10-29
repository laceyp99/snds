"""The ``separate`` job: validated input -> separation -> normalization -> BPM -> output
validation -> atomic cache publication, with staged progress and cooperative cancellation."""

from __future__ import annotations

import errno
import logging
import os
import platform
import sys
import time
from dataclasses import dataclass, field
from importlib import metadata
from typing import Any

import numpy as np
import soundfile as sf

from snds_worker import __version__, faults
from snds_worker.audio import (
    MODEL_SR,
    RESAMPLER,
    DecodedSource,
    decode_source,
    fit_length,
    peak_dbfs,
    resample,
    to_stereo,
)
from snds_worker.bpm import LOW_CONFIDENCE, analyze_bpm
from snds_worker.cache import (
    MANIFEST_FILE,
    MANIFEST_KIND,
    MANIFEST_SCHEMA_VERSION,
    PIPELINE_ID,
    TEST_PIPELINE_ID,
    JobDir,
    cache_key,
    publish_package,
    write_json_atomic,
)
from snds_worker.compute import DeviceChoice, select_device
from snds_worker.control import CancelToken
from snds_worker.logsetup import make_formatter
from snds_worker.model import SOURCES, sha256_file
from snds_worker.paths import DataDirs
from snds_worker.protocol import (
    PROTOCOL_VERSION,
    Emitter,
    ErrorCode,
    ExitCode,
    JobCancelled,
    JobRequest,
    WarningCode,
    WorkerError,
    utc_now,
)
from snds_worker.separator import FakeSeparator, make_separator
from snds_worker.validation import validate_stems

log = logging.getLogger(__name__)

# (id, label, weight of overall progress). Weights reflect CPU timings; UI estimates only.
STAGES: tuple[tuple[str, str, float], ...] = (
    ("validating-input", "Checking the source file", 0.05),
    ("loading-model", "Loading the separation model", 0.03),
    ("separating", "Separating stems", 0.75),
    ("normalizing", "Writing stems", 0.07),
    ("analyzing-bpm", "Analyzing tempo", 0.03),
    ("validating-output", "Validating stems", 0.06),
    ("finalizing", "Finalizing the cache entry", 0.01),
)
STAGE_IDS = tuple(s[0] for s in STAGES)
PROGRESS_MIN_INTERVAL = 0.25


class StageTracker:
    def __init__(self, emitter: Emitter) -> None:
        self.emitter = emitter
        self.index = -1
        self.current: str | None = None
        self.timings: dict[str, float] = {}
        self._t0 = 0.0
        self._last_emit = 0.0
        self._last_frac = -1.0
        self._total = sum(w for *_, w in STAGES)

    def begin(self, stage: str, detail: dict | None = None) -> None:
        self._close()
        self.index = STAGE_IDS.index(stage)
        self.current = stage
        self._t0 = time.perf_counter()
        _, label, _ = STAGES[self.index]
        self.emitter.stage(stage, self.index, len(STAGES), label)
        self._last_frac = -1.0
        self.update(0.0, detail, force=True)

    def update(self, frac: float, detail: dict | None = None, force: bool = False) -> None:
        now = time.perf_counter()
        frac = min(1.0, max(0.0, frac))
        if (
            not force
            and frac < 1.0
            and now - self._last_emit < PROGRESS_MIN_INTERVAL
            and frac - self._last_frac < 0.05
        ):
            return
        if frac <= self._last_frac and not force:
            return
        done = sum(w for *_, w in STAGES[: self.index])
        overall = (done + STAGES[self.index][2] * frac) / self._total
        self.emitter.progress(self.current or "", frac, overall, detail)
        self._last_emit = now
        self._last_frac = frac

    def _close(self) -> None:
        if self.current is not None:
            self.timings[self.current] = round(time.perf_counter() - self._t0, 3)

    def finish(self) -> None:
        self._close()


@dataclass
class JobState:
    request: JobRequest
    dirs: DataDirs
    job: JobDir
    key: str
    started_at: str = field(default_factory=utc_now)
    warnings: list[dict[str, Any]] = field(default_factory=list)


def _package_versions() -> dict[str, str | None]:
    out: dict[str, str | None] = {}
    for name in ("numpy", "scipy", "soundfile", "torch", "demucs"):
        try:
            out[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            out[name] = None
    if "torch" in sys.modules:  # includes the local build tag, e.g. 2.11.0+cu128
        out["torch"] = sys.modules["torch"].__version__
    out["libsndfile"] = sf.__libsndfile_version__
    return out


def _write_error(exc: OSError, what: str) -> WorkerError:
    full = exc.errno == errno.ENOSPC or getattr(exc, "winerror", None) in (112, 39)
    code = ErrorCode.DISK_FULL if full else ErrorCode.OUTPUT_WRITE_FAILED
    return WorkerError(code, f"Writing {what} failed: {exc.strerror or exc}", {"error": str(exc)})


class SeparateJob:
    def __init__(self, request: JobRequest, emitter: Emitter, token: CancelToken) -> None:
        self.request = request
        self.emitter = emitter
        self.token = token
        self.stages = StageTracker(emitter)
        self.state: JobState | None = None
        self._log_handler: logging.Handler | None = None

    # -- helpers ---------------------------------------------------------------------------

    def warn(self, code: WarningCode, message: str, **detail: Any) -> None:
        log.warning("%s: %s", code, message)
        if self.state is not None:
            self.state.warnings.append({"code": str(code), "message": message})
        self.emitter.warning(code, message, **detail)

    def _attach_job_log(self, job: JobDir) -> None:
        handler = logging.FileHandler(job.log_path, encoding="utf-8", delay=False)
        handler.setFormatter(make_formatter())
        logging.getLogger().addHandler(handler)
        self._log_handler = handler

    def _detach_job_log(self) -> None:
        if self._log_handler is not None:
            logging.getLogger().removeHandler(self._log_handler)
            self._log_handler.close()
            self._log_handler = None

    # -- entry point -----------------------------------------------------------------------

    def run(self) -> int:
        req = self.request
        try:
            dirs = DataDirs.resolve(req.data_root)
            dirs.ensure(dirs.cache, dirs.jobs)
            if not dirs.same_volume(dirs.cache, dirs.jobs):
                raise WorkerError(
                    ErrorCode.DATA_DIR_UNAVAILABLE,
                    "cache/ and jobs/ must be on the same volume for atomic publication.",
                    {"cache": str(dirs.cache), "jobs": str(dirs.jobs)},
                )
            job = JobDir.create(dirs.jobs, req.job_id)
        except WorkerError as exc:
            self.emitter.failed(exc, None)
            return int(exc.exit_code)

        key = cache_key(req.source.sha256, req.source.size_bytes)
        self.state = JobState(req, dirs, job, key)
        self._attach_job_log(job)
        job.write_status(
            "running", cache_key=key, source_path=str(req.source.path), stage=None, exit_code=None
        )
        log.info("job %s started (pid %d), cache key %s", req.job_id, os.getpid(), key)
        try:
            payload = self._run_stages()
        except JobCancelled as exc:
            return self._finish_cancelled(exc.reason)
        except KeyboardInterrupt:
            return self._finish_cancelled("interrupt")
        except WorkerError as exc:
            return self._finish_failed(exc)
        except MemoryError:
            return self._finish_failed(WorkerError(ErrorCode.OUT_OF_MEMORY, "Out of memory"))
        except Exception as exc:
            log.exception("unexpected error")
            return self._finish_failed(
                WorkerError(
                    ErrorCode.INTERNAL_ERROR,
                    f"Unexpected worker error: {type(exc).__name__}: {exc}",
                    {"exception": type(exc).__name__},
                )
            )
        # Success: the entry is published. Remove the job directory (logs went to stderr).
        self.stages.finish()
        job.write_status("published", stage=None, exit_code=0)
        self._detach_job_log()
        if not job.remove():
            log.warning("could not remove the job directory %s", job.path)
        payload["timings"] = self.stages.timings
        self.emitter.result(**payload)
        return int(ExitCode.OK)

    def _finish_failed(self, exc: WorkerError) -> int:
        stage = self.stages.current
        log.error("job failed in %s: %s (%s)", stage, exc.message, exc.code)
        assert self.state is not None
        cleaned = self.state.job.remove_incomplete()
        self.state.job.write_status(
            "failed",
            stage=stage,
            exit_code=int(exc.exit_code),
            error={"code": str(exc.code), "message": exc.message},
        )
        self._detach_job_log()
        if not cleaned:
            exc.detail.setdefault("cleanup", "incomplete")
        self.emitter.failed(exc, stage)
        return int(exc.exit_code)

    def _finish_cancelled(self, reason: str) -> int:
        stage = self.stages.current
        log.info("job cancelled during %s (%s)", stage, reason)
        assert self.state is not None
        cleaned = self.state.job.remove_incomplete()
        self.state.job.write_status(
            "cancelled", stage=stage, exit_code=int(ExitCode.CANCELLED), reason=reason
        )
        self._detach_job_log()
        self.emitter.cancelled(
            reason, stage, job_dir=str(self.state.job.path), incomplete_audio_removed=cleaned
        )
        return int(ExitCode.CANCELLED)

    def _stage(self, stage: str, detail: dict | None = None) -> None:
        self.token.check()
        self.stages.begin(stage, detail)
        assert self.state is not None
        self.state.job.write_status("running", stage=stage)
        log.info("stage %s", stage)

    # -- stages ----------------------------------------------------------------------------

    def _run_stages(self) -> dict[str, Any]:
        st = self.state
        assert st is not None
        req = st.request
        check = self.token.check

        self._stage("validating-input")
        source, source_stat = self._validate_input()

        self._stage("loading-model")
        separator = make_separator(st.dirs.models)
        fake = isinstance(separator, FakeSeparator)
        choice = self._select_device(fake)
        try:
            separator.load()
        except ImportError as exc:
            raise WorkerError(ErrorCode.DEPENDENCY_MISSING, f"Missing dependency: {exc}") from exc
        self.stages.update(1.0)

        stereo = to_stereo(source.audio)
        source.audio = None
        mix44 = np.ascontiguousarray(resample(stereo, source.samplerate, MODEL_SR).T)
        seg_total = separator.segment_count(mix44.shape[1])
        self._stage("separating", {"segments_done": 0, "segments_total": seg_total})

        def on_segment(done: int, total: int, state: str) -> None:
            check()
            if state == "end":
                self.stages.update(done / total, {"segments_done": done, "segments_total": total})

        stems44 = separator.separate(mix44, choice.selected, on_segment)
        check()

        self._stage("normalizing")
        peaks = self._normalize(stems44, source)
        drums_mono = stems44[0].mean(axis=0)
        mix_mono = mix44.mean(axis=0)
        del stems44, mix44

        self._stage("analyzing-bpm")
        bpm = analyze_bpm(drums_mono, mix_mono, MODEL_SR)
        del drums_mono, mix_mono
        if bpm["value"] is None:
            self.warn(WarningCode.BPM_UNAVAILABLE, f"No tempo could be detected ({bpm['status']}).")
        elif bpm["confidence"] < LOW_CONFIDENCE:
            self.warn(
                WarningCode.BPM_LOW_CONFIDENCE,
                f"Tempo {bpm['value']} BPM has low confidence ({bpm['confidence']}).",
            )
        self.stages.update(1.0)

        self._stage("validating-output")
        report = validate_stems(
            st.job.package / "stems",
            source=stereo,
            samplerate=source.samplerate,
            check=check,
            progress=lambda f: self.stages.update(f),
        )
        del stereo
        if not report.ok:
            raise WorkerError(
                ErrorCode.OUTPUT_VALIDATION_FAILED,
                "The separated stems failed validation: "
                + "; ".join(f["message"] for f in report.failures[:3]),
                {"failures": report.failures, "metrics": report.metrics},
            )
        for w in report.warnings:
            code = (
                WarningCode.RESIDUAL_HIGH
                if w["code"] == "residual-high"
                else WarningCode.SUBSAMPLE_LAG
            )
            self.warn(code, w["message"])

        self._stage("finalizing")
        self._check_source_unchanged(source_stat)
        pipeline_id = TEST_PIPELINE_ID if fake else PIPELINE_ID
        manifest = self._manifest(
            source, choice, separator.describe(), report, bpm, peaks, pipeline_id
        )
        try:
            write_json_atomic(st.job.package / MANIFEST_FILE, manifest)
        except OSError as exc:
            raise _write_error(exc, "the manifest") from exc
        check()  # last cancellation point: publication below is the point of no return
        st.job.write_status("publishing", stage="finalizing")
        outcome = publish_package(
            st.job.package, st.dirs.cache, st.key, st.job.path, pipeline_id=pipeline_id
        )
        if outcome.outcome == "replaced-existing":
            self.warn(
                WarningCode.REPLACED_CACHE_ENTRY,
                f"Replaced a {outcome.replaced_state} cache entry for this song.",
                reasons=outcome.replaced_reasons[:5],
            )
        self.stages.update(1.0)
        return {
            "job_id": req.job_id,
            "cache_key": st.key,
            "cache_dir": str(st.dirs.cache / st.key),
            "manifest_path": str(st.dirs.cache / st.key / MANIFEST_FILE),
            "publish": outcome.outcome,
            "replaced_state": outcome.replaced_state,
            "pipeline_id": pipeline_id,
            "device": choice.selected,
            "bpm": {
                k: bpm[k] for k in ("status", "value", "confidence", "half", "double", "signal")
            },
            "validation_status": report.status,
            "warnings": [w["code"] for w in st.warnings],
        }

    def _validate_input(self) -> tuple[DecodedSource, os.stat_result]:
        spec = self.request.source
        path = spec.path
        try:
            st = path.stat()
        except FileNotFoundError as exc:
            raise WorkerError(
                ErrorCode.SOURCE_NOT_FOUND, "The source file was not found.", {"path": str(path)}
            ) from exc
        except OSError as exc:
            raise WorkerError(
                ErrorCode.SOURCE_UNREADABLE, f"Cannot access the source: {exc}", {"path": str(path)}
            ) from exc
        if not path.is_file():
            raise WorkerError(
                ErrorCode.SOURCE_UNREADABLE, "The source is not a file.", {"path": str(path)}
            )
        if st.st_size != spec.size_bytes:
            raise WorkerError(
                ErrorCode.SOURCE_FINGERPRINT_MISMATCH,
                "The source file size differs from the fingerprint; "
                "it changed since it was hashed.",
                {"expected_size": spec.size_bytes, "actual_size": st.st_size},
            )
        if faults.active("internal-error"):
            raise RuntimeError("injected internal error")
        size = max(1, st.st_size)
        try:
            digest = sha256_file(
                path,
                progress=lambda done: self.stages.update(0.6 * done / size, {"bytes_hashed": done}),
                check=self.token.check,
            )
        except OSError as exc:
            raise WorkerError(
                ErrorCode.SOURCE_UNREADABLE, f"Cannot read the source: {exc}", {"path": str(path)}
            ) from exc
        if digest != spec.sha256:
            raise WorkerError(
                ErrorCode.SOURCE_FINGERPRINT_MISMATCH,
                "The source content differs from its fingerprint; it changed since it was hashed.",
                {"expected_sha256": spec.sha256, "actual_sha256": digest},
            )
        self.token.check()
        source = decode_source(path)
        self._check_source_unchanged(st)
        if source.mp3 is not None and not source.mp3.get("gapless_info", False):
            self.warn(
                WarningCode.MP3_NO_GAPLESS_INFO,
                "The MP3 has no LAME/Xing gapless tag; players may add a short leading delay.",
            )
        log.info(
            "source %s/%s %d Hz %d ch, %d frames decoded (header %d)",
            source.format,
            source.subtype,
            source.samplerate,
            source.channels,
            source.frames,
            source.header_frames,
        )
        self.stages.update(1.0)
        return source, st

    def _check_source_unchanged(self, before: os.stat_result) -> None:
        try:
            now = self.request.source.path.stat()
        except OSError as exc:
            raise WorkerError(
                ErrorCode.SOURCE_CHANGED, f"The source disappeared during preparation: {exc}"
            ) from exc
        if now.st_size != before.st_size or now.st_mtime_ns != before.st_mtime_ns:
            raise WorkerError(
                ErrorCode.SOURCE_CHANGED, "The source file was modified during preparation."
            )

    def _select_device(self, fake: bool) -> DeviceChoice:
        req = self.request
        if fake:
            choice = DeviceChoice(requested=req.device, selected="cpu", reason="test-separator")
        else:
            try:
                choice = select_device(req.device)
            except ImportError as exc:
                raise WorkerError(
                    ErrorCode.DEPENDENCY_MISSING,
                    f"PyTorch is not installed ({exc}). Run scripts\\bootstrap.ps1.",
                ) from exc
        self.emitter.device(**choice.to_json())
        log.info("device %s (requested %s, reason %s)", choice.selected, req.device, choice.reason)
        if choice.reason == "cuda-probe-failed":
            self.warn(
                WarningCode.CUDA_FALLBACK,
                f"The CUDA health check failed ({choice.probe_error}); using the CPU.",
            )
        return choice

    def _normalize(self, stems44: np.ndarray, source: DecodedSource) -> dict[str, float | None]:
        """Resample each stem back to the source rate, trim/pad to the exact decoded frame
        count and write stereo float32 WAV (never clipped or normalized)."""
        stems_dir = self.state.job.package / "stems"  # type: ignore[union-attr]
        peaks: dict[str, float | None] = {}
        for i, name in enumerate(SOURCES):
            self.token.check()
            y = resample(stems44[i], MODEL_SR, source.samplerate, axis=1)
            y = fit_length(y, source.frames, axis=1)
            if name == "vocals" and faults.active("drop-frame"):
                y = y[:, :-1]
            peaks[name] = peak_dbfs(y)
            try:
                sf.write(
                    str(stems_dir / f"{name}.wav"),
                    y.T,
                    source.samplerate,
                    format="WAV",
                    subtype="FLOAT",
                )
            except (sf.LibsndfileError, RuntimeError) as exc:
                raise WorkerError(
                    ErrorCode.OUTPUT_WRITE_FAILED, f"Writing {name}.wav failed: {exc}"
                ) from exc
            except OSError as exc:
                raise _write_error(exc, f"{name}.wav") from exc
            del y
            self.stages.update((i + 1) / len(SOURCES), {"stems_written": i + 1})
        if faults.active("delete-stem"):
            (stems_dir / "vocals.wav").unlink(missing_ok=True)
        return peaks

    def _manifest(
        self,
        source: DecodedSource,
        choice: DeviceChoice,
        separator_info: dict[str, Any],
        report: Any,
        bpm: dict[str, Any],
        peaks: dict[str, float | None],
        pipeline_id: str,
    ) -> dict[str, Any]:
        st = self.state
        assert st is not None
        req = st.request
        files = {}
        for name in SOURCES:
            s = report.stems[name]
            files[name] = {
                "path": s["path"],
                "bytes": s["bytes"],
                "sha256": s["sha256"],
                "peak_dbfs": s.get("peak_dbfs", peaks.get(name)),
            }
        source_info = source.describe()
        return {
            "schema_version": MANIFEST_SCHEMA_VERSION,
            "kind": MANIFEST_KIND,
            "cache_key": st.key,
            "created_at": utc_now(),
            "pipeline": {
                "pipeline_id": pipeline_id,
                **separator_info,
                "resampler": dict(RESAMPLER),
                "stem_format": {"container": "WAV", "subtype": "FLOAT", "channels": 2},
                "test_mode": faults.test_mode_settings() or None,
            },
            "producer": {
                "worker_version": __version__,
                "protocol_version": PROTOCOL_VERSION,
                "python": platform.python_version(),
                "packages": _package_versions(),
            },
            "job": {
                "job_id": req.job_id,
                "started_at": st.started_at,
                "finished_at": utc_now(),
                "timings": dict(self.stages.timings),
            },
            "compute": choice.to_json(),
            "source": {
                "fingerprint": {"sha256": req.source.sha256, "size_bytes": req.source.size_bytes},
                "path_at_preparation": str(req.source.path),
                "file_name": req.source.path.name,
                **source_info,
                "peak_dbfs": report.metrics.get("source_peak_dbfs"),
            },
            "stems": {
                "order": list(SOURCES),
                "samplerate": source.samplerate,
                "channels": 2,
                "frames": source.frames,
                "subtype": "FLOAT",
                "files": files,
                "sum_peak_dbfs": report.metrics.get("stem_sum_peak_dbfs"),
                "samples_over_0dbfs": report.metrics.get("samples_over_0dbfs"),
            },
            "validation": report.to_json(),
            "bpm": bpm,
            "warnings": list(st.warnings),
        }
