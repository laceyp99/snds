"""Stem normalization: separator output to aligned stereo float32 WAV at the source rate.

The pipeline is the one proven in spikes/normalized-stems: the source is resampled to 44.1 kHz
with `full=True` (the model always sees the whole source), and each stem is resampled back with
an explicit `output_length` so it has exactly the source frame count. No gain, normalization,
or clipping is applied; peaks above 1.0 survive in the float32 WAV and are recorded.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

from snds_worker.decode import DecodedSource
from snds_worker.errors import JobFailure
from snds_worker.separation import MODEL_RATE

log = logging.getLogger(__name__)

# The spike measured zero padding and trimming on every supported rate. Up to this many frames
# at the source rate are tolerated (and logged); beyond it, alignment is considered broken.
MAX_ADJUSTED_FRAMES = 2


@dataclass(frozen=True)
class WrittenStem:
    name: str
    path: Path
    frames: int
    sample_rate: int
    peak: float
    padded_frames: int
    trimmed_frames: int


def to_model_input(decoded: DecodedSource) -> Any:
    """Returns a (2, model_frames) float32 tensor at 44.1 kHz (mono is duplicated)."""
    import julius
    import torch

    wav = torch.from_numpy(decoded.audio.T.copy())
    if decoded.channels == 1:
        wav = wav.expand(2, -1).contiguous()
    if decoded.sample_rate != MODEL_RATE:
        wav = julius.resample_frac(wav, decoded.sample_rate, MODEL_RATE, full=True)
    return wav.contiguous()


def to_source_rate(stem: Any, sample_rate: int, frames: int) -> tuple[np.ndarray, int, int]:
    """Resamples one (2, model_frames) stem to the source rate and exactly `frames` frames.

    Returns (float32 array (frames, 2), padded_frames, trimmed_frames).
    """
    import julius
    import torch

    if sample_rate != MODEL_RATE:
        max_length = int(np.ceil(stem.shape[-1] * sample_rate / MODEL_RATE))
        stem = julius.resample_frac(
            stem, MODEL_RATE, sample_rate, output_length=min(frames, max_length)
        )
    padded = max(0, frames - stem.shape[-1])
    trimmed = max(0, stem.shape[-1] - frames)
    if padded:
        stem = torch.nn.functional.pad(stem, (0, padded))
    stem = stem[..., :frames]
    return np.ascontiguousarray(stem.numpy().T.astype(np.float32)), padded, trimmed


def write_stem(name: str, stem: Any, decoded: DecodedSource, directory: Path) -> WrittenStem:
    """Normalizes one stem and writes `<directory>/<name>.wav` (stereo, FLOAT, source rate)."""
    audio, padded, trimmed = to_source_rate(stem, decoded.sample_rate, decoded.frames)
    if max(padded, trimmed) > MAX_ADJUSTED_FRAMES:
        raise JobFailure(
            "normalization_failed",
            "The separated audio does not line up with the source.",
            details=f"{name}: padded {padded} and trimmed {trimmed} frames to match "
            f"{decoded.frames}",
        )
    if padded or trimmed:
        log.warning(
            "Stem %s: padded %d, trimmed %d frames to the source length", name, padded, trimmed
        )
    if not np.isfinite(audio).all():
        raise JobFailure(
            "normalization_failed",
            "The separator produced invalid (NaN or infinite) samples.",
            details=f"stem {name}",
        )

    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{name}.wav"
    sf.write(str(path), audio, decoded.sample_rate, subtype="FLOAT", format="WAV")
    return WrittenStem(
        name=name,
        path=path,
        frames=int(audio.shape[0]),
        sample_rate=decoded.sample_rate,
        peak=float(np.abs(audio).max()) if audio.size else 0.0,
        padded_frames=padded,
        trimmed_frames=trimmed,
    )
