"""BPM analysis outcome for `song.json`.

A failed analysis never fails the job: it yields `value=None`, `confidence=0`, an empty
alternatives list, and a short `error`. The analyzer identity is always recorded.
"""

from __future__ import annotations

import logging
from collections.abc import Callable
from dataclasses import dataclass, field
from importlib import metadata
from pathlib import Path
from typing import Any

import numpy as np

from snds_worker import model
from snds_worker.cancel import CancelToken
from snds_worker.decode import DecodedSource
from snds_worker.errors import JobCancelled

log = logging.getLogger(__name__)

ANALYZER_NAME = "beat_this"


@dataclass
class BpmOutcome:
    value: float | None
    confidence: float
    beats: int
    alternatives: list[float] = field(default_factory=list)
    error: str | None = None

    def to_json(self) -> dict[str, Any]:
        """The `bpm` object of song.json (the user override is added by the app, never here)."""
        try:
            version = metadata.version("beat-this")
        except metadata.PackageNotFoundError:
            version = "unknown"
        return {
            "value": self.value,
            "confidence": self.confidence,
            "beats": self.beats,
            "alternatives": list(self.alternatives),
            "error": self.error,
            "analyzer": {
                "name": ANALYZER_NAME,
                "version": version,
                "checkpointSha256": model.BEAT_MODEL_SHA256,
            },
            "userOverride": None,
        }


def failed(reason: str, beats: int = 0) -> BpmOutcome:
    return BpmOutcome(value=None, confidence=0.0, beats=beats, alternatives=[], error=reason)


# --- Analysis ------------------------------------------------------------------------------
#
# beat_this 1.1.0 with the `small0` checkpoint (spikes/bpm-library): beats from the source mix,
# tempo from the beat intervals. The model, chunking and post-processing are beat_this's own;
# the chunk loop is spelled out here only to poll cancellation and report progress.

CHUNK_FRAMES = 1500
BORDER_FRAMES = 6
MIN_BEATS = 3
MIN_BPM, MAX_BPM = 20.0, 400.0
_CONSISTENT = 0.10  # intervals within 10% of the median set the tempo
_REGULAR = 0.05  # confidence: share of intervals within 5% of the median


def tempo_from_beats(beats: np.ndarray) -> tuple[float, float] | None:
    """(BPM, confidence) from beat times in seconds, or None with fewer than three beats.

    Beat trackers quantize beat times to their frame rate (20 ms for beat_this), so the median
    interval alone is coarse. Averaging every interval within 10% of the median recovers
    sub-frame precision; confidence is the share of intervals within 5% of the median.
    """
    beats = np.asarray(beats, dtype=float)
    if len(beats) < MIN_BEATS:
        return None
    intervals = np.diff(beats)
    median = float(np.median(intervals))
    if median <= 0:
        return None
    consistent = intervals[np.abs(intervals - median) <= _CONSISTENT * median]
    confidence = float(np.mean(np.abs(intervals - median) <= _REGULAR * median))
    period = float(consistent.mean()) if len(consistent) else median
    return 60.0 / period, confidence


def outcome_from_beats(beats: np.ndarray) -> BpmOutcome:
    count = len(beats)
    result = tempo_from_beats(beats)
    if result is None:
        return failed(f"too few beats detected ({count})", count)
    bpm, confidence = result
    if not MIN_BPM <= bpm <= MAX_BPM:
        return failed(f"implausible tempo detected ({bpm:.1f} BPM)", count)
    return BpmOutcome(
        value=round(bpm, 3),
        confidence=round(confidence, 4),
        beats=count,
        alternatives=[round(bpm * 0.5, 3), round(bpm * 2.0, 3)],
    )


def detect_beats(
    audio: np.ndarray,
    sample_rate: int,
    checkpoint: Path,
    device: str,
    token: CancelToken,
    progress: Callable[[float], None],
) -> np.ndarray:
    """Beat times in seconds for a (frames, channels) float32 mix."""
    import torch
    from beat_this.inference import Audio2Beats, aggregate_prediction, split_piece

    tracker = Audio2Beats(checkpoint_path=str(checkpoint), device=device, dbn=False)
    token.raise_if_cancelled()
    with torch.inference_mode():
        spect = tracker.signal2spect(audio, sample_rate)
        chunks, starts = split_piece(spect, CHUNK_FRAMES, BORDER_FRAMES, avoid_short_end=True)
        predictions = []
        for index, chunk in enumerate(chunks):
            token.raise_if_cancelled()
            out = tracker.model(chunk.unsqueeze(0))
            predictions.append({"beat": out["beat"][0], "downbeat": out["downbeat"][0]})
            progress((index + 1) / len(chunks))
        beat, downbeat = aggregate_prediction(
            predictions,
            starts,
            spect.shape[0],
            CHUNK_FRAMES,
            BORDER_FRAMES,
            "keep_first",
            spect.device,
        )
        beats, _downbeats = tracker.frames2beats(beat.float(), downbeat.float())
    return np.asarray(beats, dtype=float)


def make_analyzer(model_dir: Path):
    """The pipeline's `analyze_bpm` stage for the checkpoint in `model_dir`.

    Never raises except for cancellation: any other problem becomes a failed outcome, because
    a missing tempo must not cost the user their stems. A CUDA error is retried on the CPU.
    """
    checkpoint = model_dir / model.BEAT_MODEL_FILENAME

    def analyze(
        source: DecodedSource,
        device: str,
        token: CancelToken,
        progress: Callable[[float], None],
    ) -> BpmOutcome:
        if not model.file_matches(checkpoint, model.BEAT_MODEL_SHA256, model.BEAT_MODEL_SIZE):
            log.warning("BPM checkpoint missing or damaged: %s", checkpoint)
            return failed(
                "the BPM model is missing or damaged (run scripts\\bootstrap.ps1 to install it)"
            )

        for attempt in ["cpu"] if device == "cpu" else [device, "cpu"]:
            try:
                beats = detect_beats(
                    source.audio, source.sample_rate, checkpoint, attempt, token, progress
                )
                outcome = outcome_from_beats(beats)
                log.info(
                    "BPM on %s: %s (confidence %s, %d beats)%s",
                    attempt,
                    outcome.value,
                    outcome.confidence,
                    outcome.beats,
                    f" - {outcome.error}" if outcome.error else "",
                )
                return outcome
            except JobCancelled:
                raise
            except Exception as error:
                log.exception("BPM analysis failed on %s", attempt)
                if attempt != "cpu":
                    log.warning("Retrying the BPM analysis on the CPU")
                    continue
                return failed(f"analysis failed ({type(error).__name__}: {error})")
        raise AssertionError("unreachable")

    return analyze
