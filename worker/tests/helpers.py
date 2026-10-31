"""Synthetic audio and an in-process job harness shared by the prepare tests."""

from __future__ import annotations

import hashlib
import io
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

from snds_worker import jobs_protocol
from snds_worker.cancel import CancelToken
from snds_worker.fake_separator import FakeSeparator
from snds_worker.jobs_protocol import EventWriter, PrepareRequest, SourceInfo
from snds_worker.pipeline import PreparedJob, run_prepare


def synth_audio(rate: int, seconds: float, channels: int = 2, peak: float = 0.5, seed: int = 7):
    """Deterministic music-like audio, (frames, channels) float32: tones, clicks, and noise."""
    rng = np.random.default_rng(seed)
    frames = round(rate * seconds)
    t = np.arange(frames) / rate
    mono = (
        np.sin(2 * np.pi * 110 * t)
        + 0.6 * np.sin(2 * np.pi * 440 * t + 1.0) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.7 * t))
        + 0.3 * rng.standard_normal(frames)
    )
    clicks = np.zeros(frames)
    clicks[:: max(1, rate // 4)] = 2.0
    mono += clicks
    taps = max(2, rate // 6000)  # band-limit well below 22 kHz so resampling is near-lossless
    mono = np.convolve(mono, np.ones(taps) / taps, mode="same")
    audio = np.stack([mono, 0.8 * np.roll(mono, 3)], axis=1)[:, :channels]
    return (audio / np.abs(audio).max() * peak).astype(np.float32)


def write_wav(
    path: Path, rate: int = 44_100, seconds: float = 8.0, channels: int = 2, subtype="PCM_16", **kw
) -> Path:
    sf.write(str(path), synth_audio(rate, seconds, channels, **kw), rate, subtype=subtype)
    return path


def write_mp3(path: Path, rate: int = 44_100, seconds: float = 8.0, channels: int = 2) -> Path:
    sf.write(str(path), synth_audio(rate, seconds, channels), rate, format="MP3")
    return path


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_request(
    tmp_path: Path, source: Path, *, preference: str = "cpu", sha256: str | None = None
) -> PrepareRequest:
    job_dir = tmp_path / "jobs" / "job-1"
    job_dir.mkdir(parents=True, exist_ok=True)
    digest = sha256 or (sha256_of(source) if source.exists() else "0" * 64)
    size = source.stat().st_size if source.exists() else 0
    return PrepareRequest(
        job_id="job-1",
        source=SourceInfo(str(source), size, digest),
        job_dir=str(job_dir),
        cache_dir=str(tmp_path / "cache" / f"{digest}-{size}"),
        compute_preference=preference,
        artwork=None,
    )


class _BrokenStream(io.StringIO):
    def write(self, _text: str) -> int:
        raise BrokenPipeError("the app closed stdout")


class RecordingWriter(EventWriter):
    """An EventWriter over memory that can run a hook after each event is written."""

    def __init__(self, on_event=None, *, broken: bool = False) -> None:
        super().__init__(_BrokenStream() if broken else io.StringIO())
        self.on_event = on_event

    def emit(self, event: dict[str, Any]) -> bool:
        written = super().emit(event)
        if self.on_event:
            self.on_event(event)
        return written

    @property
    def events(self) -> list[dict[str, Any]]:
        return [json.loads(line) for line in self._stream.getvalue().splitlines()]


@dataclass
class JobRun:
    exit_code: int
    events: list[dict[str, Any]]
    job: PreparedJob | None
    request: PrepareRequest

    @property
    def types(self) -> list[str]:
        return [event["type"] for event in self.events]

    @property
    def terminal(self) -> dict[str, Any]:
        return self.events[-1]

    @property
    def stages(self) -> list[str]:
        return [event["stage"] for event in self.events if event["type"] == "stage"]

    @property
    def package(self) -> Path:
        return Path(self.request.job_dir) / "package"


def run_job(
    tmp_path: Path,
    source: Path,
    *,
    separator=None,
    preference: str = "cpu",
    device: str = "cpu",
    token: CancelToken | None = None,
    on_event=None,
    request: PrepareRequest | None = None,
    analyze_bpm=None,
    finalize=None,
    factory=None,
    writer: RecordingWriter | None = None,
) -> JobRun:
    """Runs the real pipeline with a fake separator.

    `finalize` defaults to a hook that records the PreparedJob; pass finalize="default" to use
    the pipeline's own (interim) finalizer.
    """
    request = request or make_request(tmp_path, source, preference=preference)
    writer = writer or RecordingWriter(on_event)
    token = token or CancelToken()
    captured: dict[str, PreparedJob] = {}

    def record(job: PreparedJob, _token: CancelToken) -> dict[str, Any]:
        captured["job"] = job
        return {"recorded": True}

    kwargs: dict[str, Any] = {}
    if finalize != "default":
        kwargs["finalize"] = finalize or record
    if analyze_bpm is not None:
        kwargs["analyze_bpm"] = analyze_bpm
    exit_code = run_prepare(
        request,
        writer,
        token,
        separator_factory=factory or (lambda _device, _dir: separator or FakeSeparator()),
        model_dir=tmp_path / "models",
        resolve_device=lambda _preference: device,
        **kwargs,
    )
    return JobRun(exit_code, writer.events, captured.get("job"), request)


STAGE_ORDER = list(jobs_protocol.STAGES)
