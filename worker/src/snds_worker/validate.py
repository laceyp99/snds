"""Output validation: the stem contract, checked on the files that were actually written.

Checks, per docs/protocol.md section 4 and the normalized-stems spike policy: exactly the four
stem names, each a readable stereo float32 WAV at the source sample rate with exactly the source
frame count and only finite samples (the peak is measured, never clipped); and alignment, the
sum of the stems against the source in the loudest 3 s window near its start and near its end
(integer lag 0, and no sub-sample drift between the two windows). The windows are chosen by level
because fade-ins and fade-outs sit near the noise floor, where a separator's output is not
comparable to the source.
"""

from __future__ import annotations

import logging
import math
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf

from snds_worker.cancel import CancelToken
from snds_worker.decode import DecodedSource
from snds_worker.errors import JobFailure
from snds_worker.separation import STEMS

log = logging.getLogger(__name__)

WINDOW_SECONDS = 3.0
MAX_LAG_SECONDS = 0.05
MAX_DRIFT_SAMPLES = 0.05
EDGE_FRACTION = 0.4  # the windows are searched in this share of the track at each end...
EDGE_MAX_SECONDS = 45.0  # ...but never further than this from the start or the end
MIN_REFERENCE_RMS = 1e-3  # windows quieter than -60 dBFS cannot be aligned reliably
MIN_COEFFICIENT = 0.5  # peak normalized correlation between the sum of stems and the source
RESIDUAL_WARNING_DB = -20.0
_READ_FRAMES = 1 << 20

Progress = Callable[[float], None]


@dataclass(frozen=True)
class StemInfo:
    name: str
    file: str
    frames: int
    sample_rate: int
    channels: int
    peak: float


@dataclass
class ValidationReport:
    stems: dict[str, StemInfo]
    alignment: dict[str, float | int | str | None] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def _fail(message: str, details: str | None = None) -> JobFailure:
    return JobFailure("validation_failed", message, details=details)


def xcorr_lag(reference: np.ndarray, test: np.ndarray, max_lag: int) -> tuple[int, float, float]:
    """Lag of `test` relative to `reference` (positive = test is late).

    Returns the integer peak, a parabolic sub-sample estimate, and the peak's normalized
    correlation coefficient.
    """
    n = len(reference) + len(test)
    nfft = 1 << (n - 1).bit_length()
    spectrum = np.fft.rfft(test, nfft) * np.conj(np.fft.rfft(reference, nfft))
    corr = np.fft.irfft(spectrum, nfft)
    max_lag = min(max_lag, len(reference) - 1, len(test) - 1)
    window = np.concatenate((corr[-max_lag:], corr[: max_lag + 1])) if max_lag else corr[:1]
    peak = int(np.argmax(window))
    lag = peak - max_lag
    sub = float(lag)
    if 0 < peak < len(window) - 1:
        left, centre, right = window[peak - 1 : peak + 2]
        denominator = left - 2 * centre + right
        if denominator != 0:
            sub = lag + 0.5 * (left - right) / denominator
    norm = math.sqrt(float(np.dot(reference, reference)) * float(np.dot(test, test)))
    return lag, sub, float(window[peak] / norm) if norm > 0 else 0.0


def _read_stem(path: Path, source: DecodedSource, token: CancelToken):
    """Opens one stem, verifies its format, and returns (info, finite+peak scan)."""
    name = path.stem
    try:
        info = sf.info(str(path))
    except (sf.LibsndfileError, RuntimeError, OSError) as error:
        raise _fail(f"The {name} stem cannot be read.", str(error)) from error

    problems = []
    if info.format not in ("WAV", "WAVEX", "RF64"):
        problems.append(f"format {info.format}")
    if info.subtype != "FLOAT":
        problems.append(f"sample format {info.subtype}")
    if info.channels != 2:
        problems.append(f"{info.channels} channels")
    if info.samplerate != source.sample_rate:
        problems.append(f"sample rate {info.samplerate} instead of {source.sample_rate}")
    if info.frames != source.frames:
        problems.append(f"{info.frames} frames instead of {source.frames}")
    if problems:
        raise _fail(f"The {name} stem does not match the stem format.", "; ".join(problems))

    peak = 0.0
    seen = 0
    try:
        with sf.SoundFile(str(path)) as file:
            while True:
                token.raise_if_cancelled()
                block = file.read(_READ_FRAMES, dtype="float32", always_2d=True)
                if not len(block):
                    break
                if not np.isfinite(block).all():
                    raise _fail(f"The {name} stem contains invalid (NaN or infinite) samples.")
                peak = max(peak, float(np.abs(block).max()))
                seen += len(block)
    except (sf.LibsndfileError, RuntimeError, OSError) as error:
        raise _fail(f"The {name} stem cannot be read.", str(error)) from error

    if seen != source.frames:
        raise _fail(f"The {name} stem is truncated.", f"read {seen} of {source.frames} frames")

    return StemInfo(
        name=name, file=path.name, frames=seen, sample_rate=info.samplerate, channels=2, peak=peak
    )


def _window_sum(directory: Path, start: int, stop: int) -> np.ndarray:
    """Mono sum of all four stems over [start, stop), as float64."""
    total = np.zeros(stop - start, dtype=np.float64)
    for name in STEMS:
        audio, _rate = sf.read(
            str(directory / f"{name}.wav"),
            start=start,
            stop=stop,
            dtype="float32",
            always_2d=True,
        )
        total += audio.astype(np.float64).mean(axis=1)
    return total


def pick_window(region: np.ndarray, window: int, rate: int) -> tuple[int, float]:
    """The loudest `window`-long span of a mono region, as (offset in the region, RMS)."""
    if len(region) <= window:
        return 0, math.sqrt(float(np.mean(region**2))) if len(region) else 0.0

    cumulative = np.concatenate(([0.0], np.cumsum(region**2)))
    starts = np.unique(
        np.append(np.arange(0, len(region) - window + 1, max(1, rate // 2)), len(region) - window)
    )
    energy = cumulative[starts + window] - cumulative[starts]
    best = int(np.argmax(energy))
    return int(starts[best]), math.sqrt(float(energy[best]) / window)


def _check_window(
    label: str,
    reference: np.ndarray,
    directory: Path,
    start: int,
    rate: int,
    rms: float,
    report: ValidationReport,
) -> tuple[int | None, float | None]:
    """Measures one window; returns (lag, sub-sample lag), or (None, None) if not measurable."""
    report.alignment[f"{label}Seconds"] = round(start / rate, 3)
    if rms < MIN_REFERENCE_RMS:
        report.alignment[f"{label}Lag"] = None
        report.warnings.append(
            f"alignment not measured near the {label}: the source is too quiet there"
        )
        return None, None

    summed = _window_sum(directory, start, start + len(reference))
    lag, sub, coefficient = xcorr_lag(reference, summed, int(MAX_LAG_SECONDS * rate))
    report.alignment[f"{label}Lag"] = lag
    report.alignment[f"{label}Coefficient"] = round(coefficient, 4)

    residual = float(np.sum((summed - reference) ** 2))
    energy = float(np.sum(reference**2))
    residual_db = 10 * math.log10(max(residual, 1e-30) / energy)
    report.alignment[f"{label}ResidualDb"] = round(residual_db, 2)
    if residual_db > RESIDUAL_WARNING_DB:
        report.warnings.append(
            f"sum of stems differs from the source by {residual_db:.1f} dB near the {label}"
        )

    if coefficient < MIN_COEFFICIENT:
        raise _fail(
            "The separated stems do not match the source.",
            f"{label} window at {start / rate:.1f} s: correlation {coefficient:.3f} "
            f"(minimum {MIN_COEFFICIENT})",
        )
    if lag != 0:
        raise _fail(
            "The separated stems are not aligned with the source.",
            f"{label} window at {start / rate:.1f} s: lag {lag} samples at {rate} Hz",
        )
    return lag, sub


def validate_stems(
    directory: Path, source: DecodedSource, token: CancelToken, progress: Progress
) -> ValidationReport:
    """Validates `<directory>/{drums,bass,other,vocals}.wav` against the decoded source."""
    present = sorted(p.name for p in directory.glob("*.wav"))
    expected_names = sorted(f"{name}.wav" for name in STEMS)
    if present != expected_names:
        raise _fail(
            "The prepared stems are not exactly drums, bass, other and vocals.",
            f"found {present}",
        )

    report = ValidationReport(stems={})
    for index, name in enumerate(STEMS):
        report.stems[name] = _read_stem(directory / f"{name}.wav", source, token)
        progress(0.1 + 0.6 * (index + 1) / len(STEMS))

    rate = source.sample_rate
    window = min(source.frames, int(WINDOW_SECONDS * rate))
    edge = min(
        source.frames, max(window, int(min(EDGE_FRACTION * source.frames, EDGE_MAX_SECONDS * rate)))
    )

    def mono(start: int, stop: int) -> np.ndarray:
        return source.audio[start:stop].astype(np.float64).mean(axis=1)

    token.raise_if_cancelled()
    offset, rms = pick_window(mono(0, edge), window, rate)
    first_start = offset
    _, start_sub = _check_window(
        "start", mono(offset, offset + window), directory, offset, rate, rms, report
    )
    progress(0.85)

    token.raise_if_cancelled()
    offset, rms = pick_window(mono(source.frames - edge, source.frames), window, rate)
    last_start = source.frames - edge + offset
    _, end_sub = _check_window(
        "end", mono(last_start, last_start + window), directory, last_start, rate, rms, report
    )

    if start_sub is not None and end_sub is not None and last_start >= first_start + window:
        drift = end_sub - start_sub
        report.alignment["driftSamples"] = round(drift, 4)
        if abs(drift) >= MAX_DRIFT_SAMPLES:
            raise _fail(
                "The separated stems drift against the source.",
                f"drift {drift:.4f} samples between {first_start / rate:.1f} s and "
                f"{last_start / rate:.1f} s",
            )

    for warning in report.warnings:
        log.warning("Validation: %s", warning)
    log.info("Validation passed: %s", report.alignment)
    progress(1.0)
    return report
