"""Separators: the pinned htdemucs model and a deterministic fake used by routine tests.

Both take a (2, N) float32 mix at 44.1 kHz and return (4, 2, N) float32 stems in
``SOURCES`` order. ``on_segment(done, total, state)`` is called at every segment boundary
(``state`` is "start" or "end") and is the cancellation point: raising from it aborts the
remaining segments.
"""

from __future__ import annotations

import math
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

import numpy as np

from snds_worker import faults
from snds_worker.model import MODEL_SAMPLERATE, SOURCES, describe, load_model
from snds_worker.protocol import ErrorCode, JobCancelled, WorkerError

SegmentCallback = Callable[[int, int, str], None]

#: Separation parameters of pipeline 1 (docs/spikes.md: deterministic, cache-stable).
SEPARATION_PARAMS = {"shifts": 0, "split": True, "overlap": 0.25, "input_standardization": True}


def _cuda_failure(exc: BaseException, stage: str) -> WorkerError:
    text = str(exc).strip().splitlines()
    return WorkerError(
        ErrorCode.CUDA_FAILED,
        f"CUDA failed during {stage}: {text[0] if text else type(exc).__name__}",
        {"exception": type(exc).__name__},
    )


class DemucsSeparator:
    name = "htdemucs"

    def __init__(self, models_root: Path) -> None:
        self.models_root = models_root
        self.model = None

    def describe(self) -> dict[str, Any]:
        seg = float(self.model.segment) if self.model is not None else 7.8
        return {
            "separator": self.name,
            "model": describe(),
            "separation": {**SEPARATION_PARAMS, "segment_seconds": seg},
        }

    def load(self) -> None:
        self.model = load_model(self.models_root)

    def segment_count(self, frames: int) -> int:
        # Mirrors demucs.apply.apply_model(split=True): offsets = range(0, length, stride).
        segment_length = int(MODEL_SAMPLERATE * float(self.model.segment))
        stride = int((1 - SEPARATION_PARAMS["overlap"]) * segment_length)
        return max(1, len(range(0, frames, stride)))

    def separate(self, mix: np.ndarray, device: str, on_segment: SegmentCallback) -> np.ndarray:
        import torch
        from demucs.apply import apply_model

        frames = mix.shape[1]
        total = self.segment_count(frames)
        done = 0
        fail_after = (
            faults.argument("device-failure", 2) if faults.active("device-failure") else None
        )
        on_cuda = device.startswith("cuda")

        def callback(info: dict) -> None:
            nonlocal done
            state = info.get("state", "")
            if state == "end":
                done += 1
                if on_cuda and fail_after is not None and done >= fail_after:
                    raise RuntimeError("CUDA error: unspecified launch failure (injected)")
            on_segment(done, total, state)

        wav = torch.from_numpy(np.ascontiguousarray(mix, dtype=np.float32))
        # Same global standardisation as demucs.api.Separator; the mean goes back once in total.
        ref = wav.mean(0)
        mean = float(ref.mean())
        std = float(ref.std()) + 1e-8
        inp = ((wav - mean) / std)[None]
        try:
            with torch.inference_mode():
                out = apply_model(
                    self.model,
                    inp,
                    shifts=SEPARATION_PARAMS["shifts"],
                    split=SEPARATION_PARAMS["split"],
                    overlap=SEPARATION_PARAMS["overlap"],
                    progress=False,
                    device=device,
                    callback=callback,
                )
                stems = out[0]
                stems.mul_(std).add_(mean / len(SOURCES))
                return stems.numpy()
        except JobCancelled:
            raise
        except torch.OutOfMemoryError as exc:
            if on_cuda:
                raise _cuda_failure(exc, "separation") from exc
            raise WorkerError(ErrorCode.OUT_OF_MEMORY, "Not enough memory for separation") from exc
        except MemoryError as exc:
            raise WorkerError(ErrorCode.OUT_OF_MEMORY, "Not enough memory for separation") from exc
        except RuntimeError as exc:
            if on_cuda:
                raise _cuda_failure(exc, "separation") from exc
            raise WorkerError(ErrorCode.SEPARATION_FAILED, f"Separation failed: {exc}") from exc


class FakeSeparator:
    """Deterministic, torch-free stand-in: four scaled copies of the mix (sum == mix)."""

    name = "fake-test"
    GAINS = (0.4, 0.3, 0.2, 0.1)
    SEGMENT_FRAMES = MODEL_SAMPLERATE

    def describe(self) -> dict[str, Any]:
        return {
            "separator": self.name,
            "model": {"name": "fake-test", "sources": list(SOURCES)},
            "separation": {"gains": list(self.GAINS), "segment_seconds": 1.0},
        }

    def load(self) -> None:
        pass

    def segment_count(self, frames: int) -> int:
        return max(1, math.ceil(frames / self.SEGMENT_FRAMES))

    def separate(self, mix: np.ndarray, device: str, on_segment: SegmentCallback) -> np.ndarray:
        frames = mix.shape[1]
        total = self.segment_count(frames)
        delay = faults.segment_delay()
        fail_after = (
            faults.argument("device-failure", 2) if faults.active("device-failure") else None
        )
        out = np.empty((len(SOURCES), 2, frames), dtype=np.float32)
        for i in range(total):
            on_segment(i, total, "start")
            a, b = i * self.SEGMENT_FRAMES, min(frames, (i + 1) * self.SEGMENT_FRAMES)
            if delay:
                time.sleep(delay)
            for k, g in enumerate(self.GAINS):
                out[k, :, a:b] = mix[:, a:b] * np.float32(g)
            if fail_after is not None and i + 1 >= fail_after:
                raise WorkerError(
                    ErrorCode.CUDA_FAILED,
                    "CUDA failed during separation: injected device failure",
                    {"exception": "InjectedFault"},
                )
            on_segment(i + 1, total, "end")
        if faults.active("nan-output"):
            out[3, 0, frames // 2] = np.nan
        return out


def make_separator(models_root: Path) -> DemucsSeparator | FakeSeparator:
    if faults.fake_separator_enabled():
        return FakeSeparator()
    return DemucsSeparator(models_root)
