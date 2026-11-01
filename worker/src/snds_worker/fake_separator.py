"""A test double for the separator: splits the mix by fixed weights that sum to one.

The four stems add back up to the mix exactly (zero lag, no residual), so the real normalization
and validation code runs against it. It needs neither the model nor the network. Automated
tests use it in-process, and subprocess tests select it with environment variables:

- SNDS_FAKE_SEPARATOR=1      use this separator instead of htdemucs
- SNDS_FAKE_SEGMENTS=N       number of "segments" the separation is split into (default 4)
- SNDS_FAKE_SEGMENT_MS=N     milliseconds each segment takes (default 0)
- SNDS_FAKE_FAIL=KIND        fail on the first segment: oom, cuda, or error
- SNDS_FAKE_DEVICE=cuda      report "cuda" for compute.preference "auto" (default "cpu")
- SNDS_FAKE_CHATTER=1        write to stdout like a chatty library (it must never reach the app)
"""

from __future__ import annotations

import os
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from snds_worker.cancel import CancelToken
from snds_worker.separation import Progress, Separator

ENV_ENABLE = "SNDS_FAKE_SEPARATOR"
WEIGHTS = (0.4, 0.3, 0.2, 0.1)


def enabled(environ: Mapping[str, str] | None = None) -> bool:
    env = os.environ if environ is None else environ
    return env.get(ENV_ENABLE) == "1"


class FakeSeparator:
    def __init__(
        self,
        *,
        segments: int = 4,
        segment_seconds: float = 0.0,
        fail: str | None = None,
    ) -> None:
        self.segments = segments
        self.segment_seconds = segment_seconds
        self.fail = fail

    @classmethod
    def from_environment(cls, environ: Mapping[str, str] | None = None) -> FakeSeparator:
        env = os.environ if environ is None else environ
        return cls(
            segments=int(env.get("SNDS_FAKE_SEGMENTS", "4")),
            segment_seconds=float(env.get("SNDS_FAKE_SEGMENT_MS", "0")) / 1000,
            fail=env.get("SNDS_FAKE_FAIL") or None,
        )

    def separate(self, wav: Any, progress: Progress, token: CancelToken) -> Any:
        import torch

        if os.environ.get("SNDS_FAKE_CHATTER") == "1":
            print("chatter: print() to sys.stdout")
            os.write(1, b"chatter: os.write(1)\n")

        for index in range(self.segments):
            token.raise_if_cancelled()
            if self.fail == "oom":
                raise RuntimeError("CUDA out of memory. Tried to allocate 2.00 GiB (fake)")
            if self.fail == "cuda":
                raise RuntimeError("CUDA error: an illegal memory access was encountered (fake)")
            if self.fail == "error":
                raise ValueError("fake separator failure")
            if self.segment_seconds and token.wait(self.segment_seconds):
                token.raise_if_cancelled()
            progress((index + 1) / self.segments)
        return torch.stack([wav * weight for weight in WEIGHTS])


def make_fake_separator(_device: str, _model_dir: Path) -> Separator:
    return FakeSeparator.from_environment()
