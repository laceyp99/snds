"""Pinned htdemucs checkpoint: verification, hash-verified loading and acquisition.

Never uses torch.hub or the Hugging Face cache. Layout (below the SNDS data root)::

    models/htdemucs/955717e8-8726e21a.th   pinned size + SHA-256
    models/htdemucs/htdemucs.yaml          "models: ['955717e8']" (demucs bag description)
    models/htdemucs/.snds-verified.json    memo: size + mtime_ns of the last full SHA-256 check
"""

from __future__ import annotations

import hashlib
import io
import json
import logging
import os
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

from snds_worker.protocol import ErrorCode, JobCancelled, WorkerError, utc_now

log = logging.getLogger(__name__)

MODEL_NAME = "htdemucs"
MODEL_SIGNATURE = "955717e8"
CHECKPOINT_FILE = "955717e8-8726e21a.th"
CHECKPOINT_URL = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/" + CHECKPOINT_FILE
CHECKPOINT_SIZE = 84_141_911
CHECKPOINT_SHA256 = "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"
BAG_YAML = "models: ['955717e8']\n"
MODEL_SAMPLERATE = 44100
MODEL_CHANNELS = 2
SOURCES = ("drums", "bass", "other", "vocals")
MEMO_FILE = ".snds-verified.json"

HASH_CHUNK = 1 << 20


def model_dir(models_root: Path) -> Path:
    return models_root / MODEL_NAME


def checkpoint_path(models_root: Path) -> Path:
    return model_dir(models_root) / CHECKPOINT_FILE


def describe() -> dict[str, Any]:
    return {
        "name": MODEL_NAME,
        "signature": MODEL_SIGNATURE,
        "checkpoint_file": CHECKPOINT_FILE,
        "checkpoint_sha256": CHECKPOINT_SHA256,
        "checkpoint_size": CHECKPOINT_SIZE,
        "url": CHECKPOINT_URL,
        "samplerate": MODEL_SAMPLERATE,
        "channels": MODEL_CHANNELS,
        "sources": list(SOURCES),
    }


def sha256_file(
    path: Path,
    progress: Callable[[int], None] | None = None,
    check: Callable[[], None] | None = None,
) -> str:
    h = hashlib.sha256()
    done = 0
    with path.open("rb") as f:
        while block := f.read(HASH_CHUNK):
            h.update(block)
            done += len(block)
            if check is not None:
                check()
            if progress is not None:
                progress(done)
    return h.hexdigest()


@dataclass
class CheckpointStatus:
    path: Path
    present: bool
    size: int | None = None
    sha256: str | None = None
    verified_via: str | None = None  # "memo" | "hash"
    ok: bool = False
    problem: str | None = None

    def to_json(self) -> dict[str, Any]:
        return {
            "path": str(self.path),
            "present": self.present,
            "size": self.size,
            "sha256": self.sha256,
            "verified_via": self.verified_via,
            "ok": self.ok,
            "problem": self.problem,
            "expected_size": CHECKPOINT_SIZE,
            "expected_sha256": CHECKPOINT_SHA256,
        }


def _read_memo(memo: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(memo.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def write_memo(path: Path, st: os.stat_result, digest: str) -> None:
    memo = path.parent / MEMO_FILE
    payload = {
        "schema_version": 1,
        "file": path.name,
        "size": st.st_size,
        "mtime_ns": st.st_mtime_ns,
        "sha256": digest,
        "verified_at": utc_now(),
    }
    tmp = memo.with_name(memo.name + f".tmp-{os.getpid()}")
    try:
        tmp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        os.replace(tmp, memo)
    except OSError as exc:  # the memo is an optimisation only
        log.warning("could not write model verification memo: %s", exc)
        tmp.unlink(missing_ok=True)


def verify_checkpoint(models_root: Path, *, use_memo: bool = True) -> CheckpointStatus:
    """Always reports size + SHA-256. A full hash is skipped only when the memo written by the
    last full verification still matches the file's size and mtime."""
    path = checkpoint_path(models_root)
    try:
        st = path.stat()
    except FileNotFoundError:
        return CheckpointStatus(path, present=False, problem="missing")
    except OSError as exc:
        return CheckpointStatus(path, present=False, problem=f"unreadable: {exc}")
    status = CheckpointStatus(path, present=True, size=st.st_size)
    if st.st_size != CHECKPOINT_SIZE:
        status.problem = f"size {st.st_size} != pinned {CHECKPOINT_SIZE}"
        return status
    if use_memo:
        memo = _read_memo(path.parent / MEMO_FILE)
        if (
            memo
            and memo.get("file") == path.name
            and memo.get("size") == st.st_size
            and memo.get("mtime_ns") == st.st_mtime_ns
            and memo.get("sha256") == CHECKPOINT_SHA256
        ):
            status.sha256 = CHECKPOINT_SHA256
            status.verified_via = "memo"
            status.ok = True
            return status
    try:
        digest = sha256_file(path)
    except OSError as exc:
        status.problem = f"unreadable: {exc}"
        return status
    status.sha256 = digest
    status.verified_via = "hash"
    if digest != CHECKPOINT_SHA256:
        status.problem = "sha256 mismatch"
        return status
    status.ok = True
    write_memo(path, st, digest)
    return status


def read_verified_checkpoint(models_root: Path) -> bytes:
    """Read the checkpoint once and verify size + full SHA-256 of exactly those bytes
    (no time-of-check/time-of-use gap between verification and loading)."""
    path = checkpoint_path(models_root)
    try:
        data = path.read_bytes()
    except FileNotFoundError as exc:
        raise WorkerError(
            ErrorCode.MODEL_MISSING,
            "The separation model is not installed. Run scripts\\bootstrap.ps1 "
            "(or: python -m snds_worker fetch-model).",
            {"path": str(path)},
        ) from exc
    except OSError as exc:
        raise WorkerError(
            ErrorCode.MODEL_LOAD_FAILED, f"Cannot read the model: {exc}", {"path": str(path)}
        ) from exc
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != CHECKPOINT_SIZE or digest != CHECKPOINT_SHA256:
        raise WorkerError(
            ErrorCode.MODEL_CORRUPT,
            "The separation model file does not match the pinned checkpoint. Run "
            "scripts\\bootstrap.ps1 to download it again.",
            {"path": str(path), "size": len(data), "sha256": digest},
        )
    return data


def _weights_only_allowlist() -> list:
    """Exactly the pickle globals the pinned checkpoint needs (from
    ``torch.serialization.get_unsafe_globals_in_checkpoint``)."""
    import numpy as np
    from demucs.htdemucs import HTDemucs

    return [
        HTDemucs,
        Fraction,
        (np._core.multiarray.scalar, "numpy.core.multiarray.scalar"),
        np.dtype,
        np.dtypes.Float64DType,
    ]


def load_model(models_root: Path):
    """Hash-verified, ``weights_only=True`` load of the pinned model. Returns a bare HTDemucs
    (the ``htdemucs`` bag holds exactly this model with weights [1, 1, 1, 1])."""
    data = read_verified_checkpoint(models_root)
    try:
        import torch
        from demucs.states import load_model as demucs_load_model
    except ImportError as exc:
        raise WorkerError(
            ErrorCode.DEPENDENCY_MISSING,
            f"torch/demucs are not installed ({exc}). Run scripts\\bootstrap.ps1.",
        ) from exc
    try:
        with torch.serialization.safe_globals(_weights_only_allowlist()):
            package = torch.load(io.BytesIO(data), map_location="cpu", weights_only=True)
        model = demucs_load_model(package)
    except Exception as exc:
        raise WorkerError(ErrorCode.MODEL_LOAD_FAILED, f"Loading the model failed: {exc}") from exc
    if (
        tuple(model.sources) != SOURCES
        or model.samplerate != MODEL_SAMPLERATE
        or model.audio_channels != MODEL_CHANNELS
    ):
        raise WorkerError(
            ErrorCode.MODEL_LOAD_FAILED,
            "The model does not have the expected sources / sample rate / channels",
            {"sources": list(model.sources), "samplerate": model.samplerate},
        )
    model.eval()
    return model


# --- acquisition ----------------------------------------------------------------------


def write_bag_yaml(models_root: Path) -> bool:
    """Write htdemucs.yaml if it is missing or different. Returns True if written."""
    target = model_dir(models_root) / f"{MODEL_NAME}.yaml"
    try:
        if target.read_text(encoding="utf-8") == BAG_YAML:
            return False
    except OSError:
        pass
    tmp = target.with_name(target.name + f".tmp-{os.getpid()}")
    tmp.write_text(BAG_YAML, encoding="utf-8", newline="\n")
    os.replace(tmp, target)
    return True


def download_file(
    url: str,
    target: Path,
    *,
    expected_size: int,
    expected_sha256: str,
    progress: Callable[[int, int], None] | None = None,
    check: Callable[[], None] | None = None,
    timeout: float = 60.0,
    attempts: int = 3,
    user_agent: str = "snds-worker",
) -> dict[str, Any]:
    """Stream ``url`` to ``<target>.part`` while hashing, verify size + SHA-256, then
    ``os.replace`` into place. A mismatch or error deletes the partial file."""
    part = target.with_name(target.name + ".part")
    last_error: str | None = None
    for attempt in range(1, attempts + 1):
        h = hashlib.sha256()
        size = 0
        try:
            req = urllib.request.Request(url, headers={"User-Agent": user_agent})
            with urllib.request.urlopen(req, timeout=timeout) as resp, part.open("wb") as f:
                headers = {
                    "last_modified": resp.headers.get("Last-Modified"),
                    "etag": resp.headers.get("ETag"),
                }
                while block := resp.read(HASH_CHUNK):
                    f.write(block)
                    h.update(block)
                    size += len(block)
                    if size > expected_size:
                        break
                    if check is not None:
                        check()
                    if progress is not None:
                        progress(size, expected_size)
                f.flush()
                os.fsync(f.fileno())
        except JobCancelled:
            part.unlink(missing_ok=True)
            raise
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            part.unlink(missing_ok=True)
            last_error = f"{type(exc).__name__}: {exc}"
            log.warning("download attempt %d/%d failed: %s", attempt, attempts, last_error)
            if attempt < attempts:
                time.sleep(min(2.0 * attempt, 5.0))
            continue
        digest = h.hexdigest()
        if size != expected_size or digest != expected_sha256:
            part.unlink(missing_ok=True)
            raise WorkerError(
                ErrorCode.MODEL_DOWNLOAD_FAILED,
                "The downloaded file does not match the pinned size/SHA-256; it was discarded.",
                {"url": url, "size": size, "sha256": digest},
            )
        os.replace(part, target)
        return {"size": size, "sha256": digest, "attempts": attempt, "http": headers}
    raise WorkerError(
        ErrorCode.MODEL_DOWNLOAD_FAILED,
        f"Downloading the model failed after {attempts} attempts: {last_error}",
        {"url": url, "error": last_error},
    )
