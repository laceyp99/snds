"""Readiness report: can this environment run separation?

Checks that the pinned packages are installed and import, that CPU inference works, which
PyTorch build is present and whether CUDA passes a health check (a small kernel in a bounded
child process), and that both checkpoints (htdemucs and the beat_this BPM model) exist with the
pinned size and SHA-256. It is read-only: it creates no directories and writes no files.
"""

from __future__ import annotations

import importlib
import json
import platform
import subprocess
import sys
import time
import traceback
from collections.abc import Mapping
from importlib import metadata
from pathlib import Path
from typing import Any

from snds_worker import __version__, model
from snds_worker.paths import DataPaths, HomeUnavailableError, data_paths
from snds_worker.protocol import message, problem

SETUP_HINT = r"Run scripts\bootstrap.ps1."

# Distribution name -> module that separation will import.
_IMPORTS = {
    "numpy": "numpy",
    "soundfile": "soundfile",
    "torch": "torch",
    "torchaudio": "torchaudio",
    "demucs": "demucs.pretrained",
    "beat-this": "beat_this.inference",
}

_CUDA_PROBE = """
import json, time
started = time.perf_counter()
import torch
x = torch.randn(256, 256, device="cuda")
y = (x @ x).sum()
torch.cuda.synchronize()
print(json.dumps({"ok": bool(torch.isfinite(y).item())}))
"""
CUDA_PROBE_TIMEOUT_SECONDS = 45.0


def _package_versions(problems: list[dict[str, str]]) -> dict[str, str | None]:
    versions: dict[str, str | None] = {}
    for name in _IMPORTS:
        try:
            versions[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            versions[name] = None
            problems.append(
                problem(
                    "package_missing",
                    f"{name} is not installed in the worker environment. {SETUP_HINT}",
                )
            )
    return versions


def _import_packages(versions: Mapping[str, str | None], problems: list[dict[str, str]]) -> None:
    for name, module in _IMPORTS.items():
        if versions[name] is None:
            continue
        try:
            importlib.import_module(module)
        except Exception as error:
            reason = f"{type(error).__name__}: {error}"
            problems.append(
                problem(
                    "import_failed",
                    f"{module} is installed but failed to import ({reason}).",
                    traceback.format_exc(),
                )
            )


def probe_cuda(timeout: float = CUDA_PROBE_TIMEOUT_SECONDS) -> dict[str, Any]:
    """Runs a small CUDA kernel in a throwaway process, so that a wedged driver is killed when
    the timeout expires instead of hanging the readiness check.

    Returns {"ok": bool, "ms": int} plus "error" when not ok.
    """
    started = time.monotonic()
    try:
        run = subprocess.run(
            [sys.executable, "-I", "-c", _CUDA_PROBE],
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            stdin=subprocess.DEVNULL,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "ms": round(timeout * 1000), "error": f"timed out after {timeout:g} s"}
    except OSError as error:
        return {"ok": False, "ms": 0, "error": f"{type(error).__name__}: {error}"}

    elapsed = round((time.monotonic() - started) * 1000)
    if run.returncode == 0:
        try:
            if json.loads(run.stdout.strip().splitlines()[-1]).get("ok") is True:
                return {"ok": True, "ms": elapsed}
        except (ValueError, IndexError):
            pass
        return {"ok": False, "ms": elapsed, "error": "the CUDA kernel produced a wrong result"}

    lines = [line for line in run.stderr.strip().splitlines() if line.strip()]
    return {"ok": False, "ms": elapsed, "error": lines[-1] if lines else f"exit {run.returncode}"}


def _compute(check_cuda: bool) -> dict[str, Any]:
    info: dict[str, Any] = {
        "torch_build": None,
        "torch_cuda_version": None,
        "cpu_available": False,
        "cuda_available": False,
        "cuda_healthy": False,
        "cuda_device": None,
        "device": None,
    }

    torch = sys.modules.get("torch")
    if torch is None:
        return info

    try:
        info["cpu_available"] = bool(torch.ones(4).sum().item() == 4)
    except Exception as error:
        info["cpu_error"] = f"{type(error).__name__}: {error}"

    cuda_version = getattr(torch.version, "cuda", None)
    info["torch_build"] = "cuda" if cuda_version else "cpu"
    info["torch_cuda_version"] = cuda_version

    if cuda_version:
        try:
            if torch.cuda.is_available():
                info["cuda_available"] = True
                info["cuda_device"] = torch.cuda.get_device_name(0)
        except Exception as error:  # a broken driver means CPU fallback, not "not ready"
            info["cuda_error"] = f"{type(error).__name__}: {error}"

    if info["cuda_available"] and check_cuda:
        probe = probe_cuda()
        info["cuda_healthy"] = probe["ok"]
        info["cuda_check_ms"] = probe["ms"]
        if not probe["ok"]:
            info["cuda_error"] = probe.get("error", "the CUDA health check failed")

    # "auto" compute uses CUDA only after the health check passed.
    info["device"] = "cuda" if info["cuda_healthy"] else "cpu"
    return info


def _checkpoint(
    *,
    name: str,
    path: Path | None,
    expected_size: int,
    expected_sha256: str,
    code_prefix: str,
    label: str,
    problems: list[dict[str, str]],
    extra: Mapping[str, Any],
) -> dict[str, Any]:
    """Verifies one checkpoint by size and full SHA-256.

    Problem codes are `<code_prefix>` plus missing, size_mismatch, hash_mismatch, or unreadable.
    """
    info: dict[str, Any] = {
        "name": name,
        **extra,
        "path": None,
        "present": False,
        "size_bytes": None,
        "expected_size_bytes": expected_size,
        "expected_sha256": expected_sha256,
        "sha256_checked": False,
        "sha256": None,
        "verified": False,
    }
    if path is None:
        return info

    info["path"] = str(path)

    try:
        size = path.stat().st_size if path.is_file() else None
    except OSError as error:
        problems.append(problem(f"{code_prefix}unreadable", f"The {label} cannot be read: {error}"))
        return info

    if size is None:
        problems.append(
            problem(f"{code_prefix}missing", f"The {label} is not installed. {SETUP_HINT}")
        )
        return info

    info["present"] = True
    info["size_bytes"] = size

    if size != expected_size:
        problems.append(
            problem(
                f"{code_prefix}size_mismatch",
                f"The {label} has {size} bytes instead of {expected_size}; it is incomplete or "
                f"corrupt. {SETUP_HINT}",
            )
        )
        return info

    try:
        digest = model.sha256_file(path)
    except OSError as error:
        problems.append(problem(f"{code_prefix}unreadable", f"The {label} cannot be read: {error}"))
        return info

    info["sha256_checked"] = True
    info["sha256"] = digest
    if digest != expected_sha256:
        problems.append(
            problem(
                f"{code_prefix}hash_mismatch",
                f"The {label} has SHA-256 {digest} instead of {expected_sha256}; it is corrupt "
                f"or not the pinned file. {SETUP_HINT}",
            )
        )
        return info

    info["verified"] = True
    return info


def _model(paths: DataPaths | None, problems: list[dict[str, str]]) -> dict[str, Any]:
    return _checkpoint(
        name=model.MODEL_NAME,
        path=model.model_path(paths) if paths else None,
        expected_size=model.MODEL_SIZE,
        expected_sha256=model.MODEL_SHA256,
        code_prefix="model_",
        label=f"{model.MODEL_NAME} checkpoint",
        problems=problems,
        extra={"signature": model.MODEL_SIGNATURE},
    )


def _bpm_model(paths: DataPaths | None, problems: list[dict[str, str]]) -> dict[str, Any]:
    return _checkpoint(
        name=model.BEAT_MODEL_NAME,
        path=model.beat_model_path(paths) if paths else None,
        expected_size=model.BEAT_MODEL_SIZE,
        expected_sha256=model.BEAT_MODEL_SHA256,
        code_prefix="bpm_model_",
        label=f"{model.BEAT_MODEL_NAME} {model.BEAT_MODEL_CHECKPOINT} checkpoint (BPM analysis)",
        problems=problems,
        extra={"checkpoint": model.BEAT_MODEL_CHECKPOINT},
    )


def _paths(paths: DataPaths | None) -> dict[str, str] | None:
    if paths is None:
        return None
    return {
        "home": str(paths.home),
        "settings": str(paths.settings_file),
        "cache": str(paths.cache),
        "jobs": str(paths.jobs),
        "logs": str(paths.logs),
        "models": str(paths.models),
    }


def check(environ: Mapping[str, str] | None = None, *, check_cuda: bool = True) -> dict[str, Any]:
    started = time.monotonic()
    problems: list[dict[str, str]] = []

    paths: DataPaths | None
    try:
        paths = data_paths(environ)
    except HomeUnavailableError as error:
        paths = None
        problems.append(problem("home_unavailable", str(error)))

    versions = _package_versions(problems)
    _import_packages(versions, problems)
    compute = _compute(check_cuda)
    if not compute["cpu_available"] and "torch" in sys.modules:
        problems.append(problem("cpu_unavailable", "PyTorch cannot run a CPU computation."))
    model_info = _model(paths, problems)
    bpm_model_info = _bpm_model(paths, problems)

    return message(
        "readiness",
        ready=not problems,
        worker={
            "version": __version__,
            "python": platform.python_version(),
            "executable": sys.executable,
        },
        packages=versions,
        compute=compute,
        model=model_info,
        bpm_model=bpm_model_info,
        paths=_paths(paths),
        problems=problems,
        elapsed_ms=round((time.monotonic() - started) * 1000),
    )
