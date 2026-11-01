"""Four-stem separation behind a small interface.

`Separator` is what the job needs; `DemucsSeparator` is the pinned htdemucs implementation, and
tests use a fake (`snds_worker.fake_separator`) so they need neither the model nor the network.
"""

from __future__ import annotations

import errno
import logging
import random
import time
import traceback
from collections.abc import Callable
from pathlib import Path
from typing import Any, Protocol

from snds_worker import model
from snds_worker.cancel import CancelToken
from snds_worker.errors import JobFailure

log = logging.getLogger(__name__)

MODEL_RATE = 44_100
STEMS = ("drums", "bass", "other", "vocals")
SILENCE_STD = 1e-8
OVERLAP = 0.25

Progress = Callable[[float], None]


class Separator(Protocol):
    """Splits a 44.1 kHz stereo mix into the four stems."""

    def separate(self, wav: Any, progress: Progress, token: CancelToken) -> Any:
        """`wav` is a (2, frames) float32 tensor at 44.1 kHz; returns a (4, 2, frames) tensor
        in STEMS order. Polls `token` and reports progress as 0..1."""
        ...


SeparatorFactory = Callable[[str, Path], Separator]


def resolve_device(preference: str) -> str:
    """`cpu` is always CPU. `auto` is CUDA only if a small kernel runs there, else CPU."""
    if preference == "cpu":
        return "cpu"

    try:
        import torch

        if not (getattr(torch.version, "cuda", None) and torch.cuda.is_available()):
            return "cpu"
        x = torch.randn(128, 128, device="cuda")
        (x @ x).sum().item()
        torch.cuda.synchronize()
        return "cuda"
    except Exception as error:  # a broken driver means CPU, never a failed job
        log.warning("CUDA is not usable (%s: %s); using the CPU.", type(error).__name__, error)
        return "cpu"


def classify_error(error: BaseException, device: str) -> JobFailure:
    """Maps a runtime error from separation or normalization to a stable failure."""
    on_cuda = device == "cuda"
    text = f"{type(error).__name__}: {error}"
    lowered = str(error).lower()
    details = "".join(traceback.format_exception(error))[-4000:]

    if isinstance(error, OSError) and error.errno == errno.ENOSPC:
        return JobFailure(
            "disk_full",
            "There is not enough free disk space to prepare this song.",
            details=details,
            retryable=True,
            suggested_action="free_disk_space",
        )

    oom_type = type(error).__name__ == "OutOfMemoryError"
    if (
        oom_type
        or isinstance(error, MemoryError)
        or "out of memory" in lowered
        or "not enough memory" in lowered
    ):
        return JobFailure(
            "out_of_memory",
            "Separation ran out of GPU memory." if on_cuda else "Separation ran out of memory.",
            details=details,
            retryable=on_cuda,
            suggested_action="retry_cpu" if on_cuda else None,
        )

    if on_cuda and any(token in lowered for token in ("cuda", "cudnn", "cublas", "nvrtc")):
        return JobFailure(
            "cuda_failed",
            "The GPU failed during separation. Try again on the CPU.",
            details=details,
            retryable=True,
            suggested_action="retry_cpu",
        )

    return JobFailure(
        "separation_failed", f"Separation failed ({text}).", details=details, retryable=False
    )


class _SegmentPool:
    """Stands in for apply_model's executor: runs each segment lazily, in order.

    Demucs 4.0.1 has no progress callback, but it submits one task per segment and then
    evaluates them in sequence, so counting those evaluations gives exact progress and a
    cancellation point at every segment boundary.
    """

    def __init__(self, total: int, progress: Progress, token: CancelToken) -> None:
        self._total = max(total, 1)
        self._done = 0
        self._progress = progress
        self._token = token

    def submit(self, func: Callable[..., Any], *args: Any, **kwargs: Any) -> _SegmentPool._Future:
        return _SegmentPool._Future(self, func, args, kwargs)

    class _Future:
        def __init__(self, pool: _SegmentPool, func: Callable[..., Any], args: Any, kwargs: Any):
            self._pool = pool
            self._call = (func, args, kwargs)

        def result(self) -> Any:
            pool = self._pool
            pool._token.raise_if_cancelled()
            func, args, kwargs = self._call
            value = func(*args, **kwargs)
            pool._done += 1
            pool._progress(min(1.0, pool._done / pool._total))
            return value


class DemucsSeparator:
    """The pinned htdemucs checkpoint through demucs.apply.apply_model.

    Mirrors the Demucs 4.0.1 CLI (mean/std standardization, split with 25% overlap, no shifts)
    minus file I/O and clip prevention, as proven in spikes/normalized-stems.
    """

    def __init__(self, device: str, model_dir: Path) -> None:
        from demucs.pretrained import get_model

        checkpoint = model_dir / model.MODEL_FILENAME
        if not model.file_matches(checkpoint, model.MODEL_SHA256, model.MODEL_SIZE):
            raise JobFailure(
                "model_missing",
                f"The {model.MODEL_NAME} checkpoint is missing or damaged. Run "
                r"scripts\bootstrap.ps1 to install it.",
                details=str(checkpoint),
                suggested_action="fetch_model",
            )

        import torch

        started = time.perf_counter()
        self._device = torch.device(device)
        self._model = get_model(model.MODEL_SIGNATURE, repo=model_dir)
        if tuple(self._model.sources) != STEMS:
            raise JobFailure(
                "separation_failed",
                "The separation model does not provide the expected four stems.",
                details=f"sources: {list(self._model.sources)}",
            )
        self._model.to(self._device).eval()
        log.info(
            "Loaded %s on %s in %.1f s", model.MODEL_NAME, device, time.perf_counter() - started
        )

    def _segment_count(self, frames: int) -> int:
        sub_model = self._model.models[0] if hasattr(self._model, "models") else self._model
        segment_length = int(sub_model.samplerate * sub_model.segment)
        stride = max(1, int((1 - OVERLAP) * segment_length))
        return -(-frames // stride)

    def separate(self, wav: Any, progress: Progress, token: CancelToken) -> Any:
        import torch
        from demucs.apply import apply_model

        reference = wav.mean(0)
        mean = reference.mean()
        std = reference.std()
        if not torch.isfinite(std) or std < SILENCE_STD:
            std = torch.tensor(1.0)  # digital silence would otherwise divide by zero

        pool = _SegmentPool(self._segment_count(wav.shape[-1]), progress, token)
        random.seed(0)
        with torch.inference_mode():
            estimate = apply_model(
                self._model,
                ((wav - mean) / std)[None],
                device=self._device,
                shifts=0,
                split=True,
                overlap=OVERLAP,
                progress=False,
                pool=pool,
            )[0]
            if self._device.type == "cuda":
                torch.cuda.synchronize(self._device)
            stems = (estimate.cpu() * std + mean).contiguous()

        if self._device.type == "cuda":
            torch.cuda.empty_cache()
        return stems


def make_demucs_separator(device: str, model_dir: Path) -> Separator:
    return DemucsSeparator(device, model_dir)


__all__ = [
    "MODEL_RATE",
    "STEMS",
    "DemucsSeparator",
    "Separator",
    "SeparatorFactory",
    "classify_error",
    "make_demucs_separator",
    "resolve_device",
]
