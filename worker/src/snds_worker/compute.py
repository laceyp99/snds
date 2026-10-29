"""Compute device selection: validated CUDA automatically, CPU fallback, force-CPU.

Policy (docs/spikes.md, verify-spike-cuda-fallback):

1. ``cpu`` is always honoured (force-CPU setting and the explicit CPU retry).
2. ``auto`` uses CUDA only if ``torch.cuda.is_available()`` and a small compute probe passes;
   otherwise it falls back to CPU and reports the reason.
3. ``cuda`` requires a validated CUDA device, else ``cuda-unavailable`` (exit 3).
4. A CUDA failure after the job started exits with ``cuda-failed`` (exit 3); the app retries on
   CPU in a *new* process, never inside one whose CUDA context may be poisoned.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass
from typing import Any

from snds_worker import faults
from snds_worker.protocol import ErrorCode, WorkerError

CUDA_DEVICE = "cuda:0"


@dataclass
class DeviceChoice:
    requested: str
    selected: str
    reason: str | None = None  # why CPU was used; None when CUDA was selected
    cuda_available: bool = False
    device_name: str | None = None
    torch_version: str | None = None
    torch_cuda: str | None = None
    probe_error: str | None = None
    threads: int | None = None

    def to_json(self) -> dict[str, Any]:
        return {
            "requested": self.requested,
            "selected": self.selected,
            "reason": self.reason,
            "cuda_available": self.cuda_available,
            "device_name": self.device_name,
            "torch_version": self.torch_version,
            "torch_cuda": self.torch_cuda,
            "probe_error": self.probe_error,
            "threads": self.threads,
        }

    @property
    def is_cuda(self) -> bool:
        return self.selected.startswith("cuda")


def _first_line(exc: BaseException) -> str:
    text = str(exc).strip().splitlines()
    return f"{type(exc).__name__}: {text[0] if text else ''}".strip()


def probe_cuda(device: str = CUDA_DEVICE) -> str | None:
    """Run a small real computation on ``device`` and compare with the CPU.
    Returns None if healthy, else a one-line reason."""
    if faults.active("cuda-probe"):
        return "injected CUDA probe failure"
    import torch

    try:
        g = torch.Generator().manual_seed(1234)
        a = torch.randn(256, 256, generator=g)
        ref = a @ a
        got = (a.to(device) @ a.to(device)).cpu()
        torch.cuda.synchronize(device)
        err = float((got - ref).abs().max() / ref.abs().max())
        if not err < 1e-3:
            return f"CUDA result differs from CPU (relative error {err:.3g})"
        return None
    except Exception as exc:
        return _first_line(exc)


def probe_cuda_bounded(device: str = CUDA_DEVICE, timeout: float = 30.0) -> tuple[str | None, bool]:
    """probe_cuda in a helper thread. Returns (error, timed_out). A hung driver call cannot be
    interrupted; the caller should exit the process promptly after a timeout."""
    box: dict[str, str | None] = {}

    def run() -> None:
        box["error"] = probe_cuda(device)

    t = threading.Thread(target=run, name="cuda-probe", daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        return f"CUDA health check did not finish within {timeout:.0f} s", True
    return box.get("error"), False


def select_device(requested: str) -> DeviceChoice:
    import torch

    choice = DeviceChoice(
        requested=requested,
        selected="cpu",
        torch_version=torch.__version__,
        torch_cuda=torch.version.cuda,
        threads=torch.get_num_threads(),
    )
    if requested == "cpu":
        choice.reason = "forced-cpu"
        choice.cuda_available = bool(torch.cuda.is_available())
        return choice
    choice.cuda_available = bool(torch.cuda.is_available())
    if not choice.cuda_available:
        if requested == "cuda":
            raise WorkerError(
                ErrorCode.CUDA_UNAVAILABLE,
                "CUDA was requested but no usable CUDA device is available.",
                {"torch_cuda": torch.version.cuda},
            )
        choice.reason = "cuda-not-available"
        return choice
    error = probe_cuda(CUDA_DEVICE)
    if error is not None:
        if requested == "cuda":
            raise WorkerError(
                ErrorCode.CUDA_UNAVAILABLE,
                f"The CUDA health check failed: {error}",
                {"probe_error": error},
            )
        choice.reason = "cuda-probe-failed"
        choice.probe_error = error
        return choice
    choice.selected = CUDA_DEVICE
    try:
        choice.device_name = torch.cuda.get_device_name(0)
    except Exception:
        choice.device_name = None
    return choice
