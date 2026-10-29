"""``readiness``: machine-readable environment checks with actionable messages.

Statuses match ``snds::ReadinessStatus``: ``ok``, ``warning`` (usable, deserves attention) and
``missing`` (blocks separation until the user acts).
"""

from __future__ import annotations

import logging
import os
import platform
import shutil
import sys
from importlib import metadata
from pathlib import Path
from typing import Any

from snds_worker import faults
from snds_worker.cache import PIPELINE_ID, TEST_PIPELINE_ID
from snds_worker.compute import CUDA_DEVICE, probe_cuda_bounded
from snds_worker.model import checkpoint_path, describe, verify_checkpoint
from snds_worker.paths import DataDirs

log = logging.getLogger(__name__)

BOOTSTRAP = "scripts\\bootstrap.ps1"
PINNED = {
    "numpy": "2.5.3",
    "scipy": "1.18.1",
    "soundfile": "0.14.0",
    "torch": "2.11.0",
    "demucs": "4.1.0",
}
LOW_DISK_BYTES = 2 << 30


def _check(
    id_: str, title: str, status: str, detail: str, action: str = "", **data: Any
) -> dict[str, Any]:
    return {
        "id": id_,
        "title": title,
        "status": status,
        "detail": detail,
        "action": action,
        "data": data,
    }


def _cpu_name() -> str:
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
        ) as k:
            return str(winreg.QueryValueEx(k, "ProcessorNameString")[0]).strip()
    except OSError:
        return platform.processor() or "unknown"


class Readiness:
    def __init__(
        self, data_root: Path | None, force_cpu: bool, full_verify: bool, probe_timeout: float
    ) -> None:
        self.data_root = data_root
        self.force_cpu = force_cpu
        self.full_verify = full_verify
        self.probe_timeout = probe_timeout
        self.checks: list[dict[str, Any]] = []
        self.probe_timed_out = False
        self.torch = None
        self.compute: dict[str, Any] = {}

    def run(self) -> dict[str, Any]:
        self._python()
        self._packages()
        self._mp3()
        dirs = self._data_dirs()
        self._model(dirs)
        self._cpu()
        self._cuda()
        test_mode = faults.test_mode_settings()
        if test_mode:
            self.checks.append(
                _check(
                    "worker-test-mode",
                    "Worker test seams",
                    "warning",
                    "Test environment variables are set: " + ", ".join(sorted(test_mode)),
                    "Unset the SNDS_WORKER_TEST_* variables; entries prepared now are unusable.",
                    settings=test_mode,
                )
            )
        return {
            "separation_available": all(c["status"] != "missing" for c in self.checks),
            "compute": self.compute,
            "pipeline_id": TEST_PIPELINE_ID if faults.fake_separator_enabled() else PIPELINE_ID,
            "model": describe(),
            "data_root": str(dirs.root) if dirs else None,
            "checks": self.checks,
        }

    def _python(self) -> None:
        v = sys.version_info
        ok = (v.major, v.minor) == (3, 12)
        self.checks.append(
            _check(
                "worker-python",
                "Worker Python",
                "ok" if ok else "warning",
                f"Python {platform.python_version()} ({sys.executable})",
                "" if ok else f"The worker is pinned to Python 3.12; re-run {BOOTSTRAP}.",
                version=platform.python_version(),
                executable=sys.executable,
            )
        )

    def _packages(self) -> None:
        versions: dict[str, str | None] = {}
        for name in PINNED:
            try:
                versions[name] = metadata.version(name)
            except metadata.PackageNotFoundError:
                versions[name] = None
        missing = [n for n, v in versions.items() if v is None]
        import_error = None
        if not missing:
            try:
                import demucs.apply  # noqa: F401
                import torch

                self.torch = torch
                versions["torch"] = torch.__version__
            except Exception as exc:
                import_error = f"{type(exc).__name__}: {exc}"
        mismatched = [
            n for n, v in versions.items() if v is not None and v.split("+")[0] != PINNED[n]
        ]
        build = None
        if versions.get("torch"):
            build = "cuda" if "+cu" in versions["torch"] else "cpu"
        if missing or import_error:
            status, action = "missing", f"Run {BOOTSTRAP} to install the worker environment."
            detail = (
                f"Missing: {', '.join(missing)}" if missing else f"Import failed: {import_error}"
            )
        elif mismatched:
            status = "warning"
            detail = "Versions differ from the lock: " + ", ".join(
                f"{n} {versions[n]}" for n in mismatched
            )
            action = f"Run {BOOTSTRAP} to re-sync the locked environment."
        else:
            status, action = "ok", ""
            detail = ", ".join(f"{n} {v}" for n, v in versions.items())
        self.checks.append(
            _check(
                "worker-packages",
                "Worker packages",
                status,
                detail,
                action,
                versions=versions,
                torch_build=build,
            )
        )

    def _mp3(self) -> None:
        import soundfile as sf

        has_mp3 = "MP3" in sf.available_formats()
        self.checks.append(
            _check(
                "mp3-decoder",
                "MP3 decoding",
                "ok" if has_mp3 else "missing",
                f"libsndfile {sf.__libsndfile_version__}"
                + ("" if has_mp3 else " without MP3 support"),
                "" if has_mp3 else f"Run {BOOTSTRAP} to reinstall soundfile.",
                libsndfile=sf.__libsndfile_version__,
            )
        )

    def _data_dirs(self) -> DataDirs | None:
        try:
            dirs = DataDirs.resolve(self.data_root)
            dirs.ensure(dirs.cache, dirs.jobs, dirs.models)
            probe = dirs.jobs / f".write-test-{os.getpid()}"
            probe.write_bytes(b"ok")
            probe.unlink()
            same = dirs.same_volume(dirs.cache, dirs.jobs)
            free = shutil.disk_usage(dirs.root).free
        except Exception as exc:
            self.checks.append(
                _check(
                    "data-directories",
                    "SNDS data folders",
                    "missing",
                    f"The data folders are not writable: {exc}",
                    "Check permissions or set SNDS_DATA_DIR to a writable absolute path.",
                )
            )
            return None
        status, action = "ok", ""
        detail = f"{dirs.root} ({free / 2**30:.1f} GB free)"
        if not same:
            status = "missing"
            action = "Keep cache and jobs on one volume (no junctions to other drives)."
            detail += "; cache and jobs are on different volumes"
        elif free < LOW_DISK_BYTES:
            status = "warning"
            action = "Free disk space: stems need about 85 MB per minute of 44.1 kHz audio."
        self.checks.append(
            _check(
                "data-directories",
                "SNDS data folders",
                status,
                detail,
                action,
                root=str(dirs.root),
                free_bytes=free,
                same_volume=same,
            )
        )
        return dirs

    def _model(self, dirs: DataDirs | None) -> None:
        if dirs is None:
            self.checks.append(
                _check(
                    "model-checkpoint",
                    "Separation model",
                    "missing",
                    "Data folders unavailable.",
                    "",
                )
            )
            return
        status = verify_checkpoint(dirs.models, use_memo=not self.full_verify)
        action = ""
        if status.ok:
            state = "ok"
            detail = f"{status.path} (SHA-256 verified via {status.verified_via})"
        elif not status.present:
            state = "missing"
            detail = f"{checkpoint_path(dirs.models)} is not installed"
            action = f"Run {BOOTSTRAP} (or: worker python -m snds_worker fetch-model)."
        else:
            state = "missing"
            detail = f"{status.path} is invalid: {status.problem}"
            action = f"Run {BOOTSTRAP} to download the pinned model again."
        self.checks.append(
            _check(
                "model-checkpoint", "Separation model", state, detail, action, **status.to_json()
            )
        )

    def _cpu(self) -> None:
        threads = self.torch.get_num_threads() if self.torch is not None else None
        logical = os.cpu_count()
        slow = threads is not None and threads < 4
        self.checks.append(
            _check(
                "cpu",
                "CPU",
                "warning" if slow else "ok",
                f"{_cpu_name()}, {logical} logical processors, {threads} compute threads",
                "CPU separation will be slow on fewer than 4 cores." if slow else "",
                name=_cpu_name(),
                logical_processors=logical,
                compute_threads=threads,
            )
        )

    def _cuda(self) -> None:
        torch = self.torch
        requested = "cpu" if self.force_cpu else "auto"
        self.compute = {
            "requested": requested,
            "selected": "cpu",
            "reason": None,
            "device_name": None,
        }
        if torch is None:
            self.compute["reason"] = "torch-unavailable"
            self.checks.append(
                _check("cuda", "CUDA acceleration", "warning", "PyTorch is not importable.", "")
            )
            return
        available = bool(torch.cuda.is_available())
        data: dict[str, Any] = {
            "torch_cuda": torch.version.cuda,
            "available": available,
            "probe": None,
        }
        if available:
            try:
                props = torch.cuda.get_device_properties(0)
                data.update(
                    device_name=props.name,
                    capability=f"{props.major}.{props.minor}",
                    total_memory_gib=round(props.total_memory / 2**30, 2),
                )
            except Exception as exc:
                data["properties_error"] = str(exc)
        if self.force_cpu:
            self.compute["reason"] = "forced-cpu"
            detail = "Force CPU is on; the CUDA health check was skipped."
            self.checks.append(_check("cuda", "CUDA acceleration", "ok", detail, "", **data))
            return
        if not available:
            self.compute["reason"] = "cuda-not-available"
            cpu_build = torch.version.cuda is None
            detail = (
                "CPU build of PyTorch; separation runs on the CPU."
                if cpu_build
                else "No usable CUDA device; separation runs on the CPU."
            )
            action = (
                ""
                if cpu_build
                else "Install/update the NVIDIA driver (R570+) for a supported GPU "
                "(GTX 16 / RTX 20 or newer)."
            )
            self.checks.append(
                _check(
                    "cuda",
                    "CUDA acceleration",
                    "ok" if cpu_build else "warning",
                    detail,
                    action,
                    **data,
                )
            )
            return
        error, timed_out = probe_cuda_bounded(CUDA_DEVICE, self.probe_timeout)
        self.probe_timed_out = timed_out
        data["probe"] = "failed" if error else "passed"
        if error:
            self.compute["reason"] = "cuda-probe-failed"
            data["probe_error"] = error
            self.checks.append(
                _check(
                    "cuda",
                    "CUDA acceleration",
                    "warning",
                    f"CUDA health check failed: {error}. Separation will use the CPU.",
                    "Update the NVIDIA driver (R570+) or enable Force CPU.",
                    **data,
                )
            )
            return
        self.compute.update(selected=CUDA_DEVICE, device_name=data.get("device_name"))
        self.checks.append(
            _check(
                "cuda",
                "CUDA acceleration",
                "ok",
                f"{data.get('device_name')} (CUDA {torch.version.cuda}) passed the health check",
                "",
                **data,
            )
        )
