"""Test seams. Inert unless the environment variables below are set.

``SNDS_WORKER_TEST_SEPARATOR=fake``
    Replace htdemucs with a deterministic, torch-free separator that splits the 44.1 kHz mix
    into four scaled copies summing to the input (1 s segments). Entries it produces carry a
    test pipeline id that the app must never accept (see docs/protocol.md).
``SNDS_WORKER_TEST_SEGMENT_DELAY=<seconds>``
    Sleep per fake segment (makes cancellation tests deterministic).
``SNDS_WORKER_TEST_FAULT=<name>[,<name>...]``
    Inject failures at fixed points:

    - ``device-failure@N``: CUDA-style failure after N segments (fake: always; htdemucs: only
      when running on CUDA). Exercises the exit-3 CPU-retry path.
    - ``cuda-probe``: the CUDA health probe fails (auto -> CPU fallback).
    - ``nan-output``: the fake separator emits a NaN sample.
    - ``drop-frame``: normalization writes the vocals stem one frame short.
    - ``delete-stem``: the vocals stem is deleted before output validation.
    - ``crash-before-publish``: hard exit (code 70) just before the cache publish rename.
    - ``crash-after-aside``: hard exit (code 70) after an old entry was renamed aside.
    - ``internal-error``: raise an unexpected exception while validating input.
"""

from __future__ import annotations

import os
import sys

SEPARATOR_ENV = "SNDS_WORKER_TEST_SEPARATOR"
SEGMENT_DELAY_ENV = "SNDS_WORKER_TEST_SEGMENT_DELAY"
FAULT_ENV = "SNDS_WORKER_TEST_FAULT"
CRASH_EXIT_CODE = 70


def _faults() -> dict[str, str]:
    out: dict[str, str] = {}
    for item in os.environ.get(FAULT_ENV, "").split(","):
        item = item.strip()
        if item:
            name, _, arg = item.partition("@")
            out[name] = arg
    return out


def active(name: str) -> bool:
    return name in _faults()


def argument(name: str, default: int) -> int:
    arg = _faults().get(name, "")
    try:
        return int(arg) if arg else default
    except ValueError:
        return default


def crash_point(name: str) -> None:
    """Simulate a hard crash (no cleanup, no terminal message) when ``name`` is injected."""
    if active(name):
        sys.stderr.write(f"injected crash at {name}\n")
        sys.stderr.flush()
        os._exit(CRASH_EXIT_CODE)


def fake_separator_enabled() -> bool:
    return os.environ.get(SEPARATOR_ENV, "").strip().lower() == "fake"


def segment_delay() -> float:
    try:
        return max(0.0, float(os.environ.get(SEGMENT_DELAY_ENV, "0") or 0))
    except ValueError:
        return 0.0


def test_mode_settings() -> dict[str, str]:
    """Non-empty when any test seam is active (reported by readiness and in manifests)."""
    return {
        k: os.environ[k]
        for k in (SEPARATOR_ENV, SEGMENT_DELAY_ENV, FAULT_ENV)
        if os.environ.get(k, "").strip()
    }
