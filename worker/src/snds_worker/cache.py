"""Cache layout, song-package manifest, job directories and atomic publication.

Layout below the data root (see docs/protocol.md)::

    cache/<cache_key>/manifest.json          written last; its presence + validity = complete
    cache/<cache_key>/stems/<stem>.wav        drums, bass, other, vocals (float32 stereo)
    cache/<cache_key>/overrides.json          app-owned (user BPM correction), optional
    jobs/<job_id>/status.json                 job state (running/failed/cancelled/published)
    jobs/<job_id>/job.log                     this job's worker log (kept on failure/cancel)
    jobs/<job_id>/package/                    becomes cache/<cache_key>/ by one directory rename

``cache_key = "<source sha256 hex>-<source size in bytes>"``.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import logging
import os
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import soundfile as sf

from snds_worker import faults
from snds_worker.model import CHECKPOINT_SHA256, SOURCES
from snds_worker.protocol import ErrorCode, WorkerError, utc_now

log = logging.getLogger(__name__)

MANIFEST_SCHEMA_VERSION = 1
MANIFEST_KIND = "snds-song-package"
MANIFEST_FILE = "manifest.json"
OVERRIDES_FILE = "overrides.json"
OVERRIDES_SCHEMA_VERSION = 1
JOB_STATUS_SCHEMA_VERSION = 1
JOB_STATUS_KIND = "snds-job-status"
MAX_MANIFEST_BYTES = 4 << 20

#: Identity of everything that determines stem bytes (checkpoint, separation parameters, input
#: standardisation, resampler, stem format). Entries with another id are "incompatible".
PIPELINE_ID = f"snds-p1-htdemucs-{CHECKPOINT_SHA256[:8]}"
TEST_PIPELINE_ID = "snds-test-fake-separator"
VALID_VALIDATION_STATUSES = ("passed", "passed-with-warnings")

CACHE_KEY_RE = re.compile(r"^[0-9a-f]{64}-(0|[1-9][0-9]*)$")


def remove_path(path: Path) -> None:
    """Best-effort removal of a file or directory tree."""
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path, ignore_errors=True)
    else:
        with contextlib.suppress(OSError):
            path.unlink()


def cache_key(sha256: str, size_bytes: int) -> str:
    return f"{sha256}-{size_bytes}"


def write_json_atomic(path: Path, obj: Any) -> None:
    """Temp file in the same directory, flush + fsync, then os.replace."""
    tmp = path.with_name(f"{path.name}.tmp-{os.getpid()}")
    data = json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + "\n"
    try:
        with tmp.open("w", encoding="utf-8", newline="\n") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise


def read_json(path: Path, max_bytes: int = MAX_MANIFEST_BYTES) -> Any:
    size = path.stat().st_size
    if size > max_bytes:
        raise ValueError(f"{path.name} is {size} bytes (limit {max_bytes})")
    return json.loads(path.read_text(encoding="utf-8"))


# --- entry inspection (the reference for the app's pre-playback validation) ------------------


@dataclass
class EntryInspection:
    state: str  # "valid" | "missing" | "invalid" | "incompatible" | "unknown-schema"
    reasons: list[str] = field(default_factory=list)
    manifest: dict[str, Any] | None = None

    @property
    def valid(self) -> bool:
        return self.state == "valid"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while block := f.read(1 << 20):
            h.update(block)
    return h.hexdigest()


def inspect_entry(
    entry: Path,
    *,
    expected_key: str | None = None,
    pipeline_id: str = PIPELINE_ID,
    deep: bool = False,
) -> EntryInspection:
    """Validate ``cache/<key>/`` exactly as docs/protocol.md requires before playback.
    ``deep`` also verifies each stem's SHA-256."""
    if not entry.exists():
        return EntryInspection("missing")
    if not entry.is_dir():
        return EntryInspection("invalid", ["entry is not a directory"])
    mpath = entry / MANIFEST_FILE
    if not mpath.is_file():
        return EntryInspection("invalid", ["manifest.json is missing (incomplete entry)"])
    try:
        m = read_json(mpath)
    except (OSError, ValueError) as exc:
        return EntryInspection("invalid", [f"manifest.json is unreadable: {exc}"])
    if not isinstance(m, dict):
        return EntryInspection("invalid", ["manifest.json is not an object"])
    version = m.get("schema_version")
    if not isinstance(version, int) or isinstance(version, bool) or version < 1:
        return EntryInspection("invalid", [f"bad schema_version {version!r}"], m)
    if version != MANIFEST_SCHEMA_VERSION:
        return EntryInspection("unknown-schema", [f"schema_version {version} is not supported"], m)
    reasons: list[str] = []
    if m.get("kind") != MANIFEST_KIND:
        reasons.append(f"kind {m.get('kind')!r} != {MANIFEST_KIND!r}")
    key = m.get("cache_key")
    if key != entry.name:
        reasons.append(f"cache_key {key!r} does not match the directory name")
    if expected_key is not None and key != expected_key:
        reasons.append(f"cache_key {key!r} != expected {expected_key!r}")
    try:
        fp = m["source"]["fingerprint"]
        if cache_key(fp["sha256"], fp["size_bytes"]) != key:
            reasons.append("source fingerprint does not match cache_key")
        source_sr = m["source"]["samplerate"]
        source_frames = m["source"]["decoded_frames"]
        stems = m["stems"]
        files = stems["files"]
        status = m["validation"]["status"]
        pid = m["pipeline"]["pipeline_id"]
    except (KeyError, TypeError) as exc:
        return EntryInspection("invalid", [*reasons, f"manifest field missing: {exc}"], m)
    if status not in VALID_VALIDATION_STATUSES:
        reasons.append(f"validation status {status!r}")
    if stems.get("samplerate") != source_sr or stems.get("frames") != source_frames:
        reasons.append("stems samplerate/frames disagree with the source")
    if stems.get("channels") != 2 or list(stems.get("order", [])) != list(SOURCES):
        reasons.append("stems channels/order are not the 4-stem stereo contract")
    for name in SOURCES:
        f = files.get(name) if isinstance(files, dict) else None
        if not isinstance(f, dict) or f.get("path") != f"stems/{name}.wav":
            reasons.append(f"manifest has no entry for {name}")
            continue
        p = entry / "stems" / f"{name}.wav"
        try:
            size = p.stat().st_size
        except OSError:
            reasons.append(f"{name}.wav is missing")
            continue
        if size != f.get("bytes"):
            reasons.append(f"{name}.wav has {size} bytes, manifest says {f.get('bytes')}")
            continue
        try:
            info = sf.info(str(p))
        except (sf.LibsndfileError, RuntimeError, OSError) as exc:
            reasons.append(f"{name}.wav is unreadable: {exc}")
            continue
        if (
            info.format != "WAV"
            or info.subtype != "FLOAT"
            or info.channels != 2
            or info.samplerate != source_sr
            or info.frames != source_frames
        ):
            reasons.append(f"{name}.wav header does not match the stem contract")
            continue
        if deep and _sha256(p) != f.get("sha256"):
            reasons.append(f"{name}.wav content hash mismatch")
    if reasons:
        return EntryInspection("invalid", reasons, m)
    if pid != pipeline_id:
        return EntryInspection("incompatible", [f"pipeline_id {pid!r} != {pipeline_id!r}"], m)
    return EntryInspection("valid", [], m)


# --- publication -------------------------------------------------------------------------


@dataclass
class PublishOutcome:
    outcome: str  # "published" | "reused-existing" | "replaced-existing"
    replaced_state: str | None = None
    replaced_reasons: list[str] = field(default_factory=list)
    overrides_carried: bool = False


def publish_package(
    package: Path,
    cache_root: Path,
    key: str,
    job_dir: Path,
    *,
    pipeline_id: str = PIPELINE_ID,
    attempts: int = 3,
) -> PublishOutcome:
    """Publish ``package`` as ``cache_root/key`` with a single directory rename.

    - No entry: rename (Windows MoveFileEx without REPLACE fails if the target exists, so a
      concurrent publisher can never be overwritten).
    - Existing entry that passes a deep check: keep it, discard ours ("reused-existing").
    - Existing invalid / incompatible / unknown-schema entry: rename it aside into this job's
      directory, then rename ours into place ("replaced-existing"). A crash between the two
      renames leaves *no* entry, never a partial one. ``overrides.json`` is carried over.
    """
    target = cache_root / key
    for attempt in range(attempts):
        faults.crash_point("crash-before-publish")
        try:
            os.rename(package, target)
            return PublishOutcome("published")
        except FileExistsError:
            pass
        except OSError as exc:
            if not target.exists():
                raise WorkerError(
                    ErrorCode.CACHE_PUBLISH_FAILED,
                    f"Publishing the cache entry failed: {exc}",
                    {"target": str(target)},
                ) from exc
        existing = inspect_entry(target, expected_key=key, pipeline_id=pipeline_id, deep=True)
        if existing.valid:
            shutil.rmtree(package, ignore_errors=True)
            return PublishOutcome("reused-existing")
        if existing.state == "missing":
            continue  # removed concurrently; retry the plain rename
        carried = False
        old_overrides = target / OVERRIDES_FILE
        if old_overrides.is_file() and not (package / OVERRIDES_FILE).exists():
            try:
                shutil.copy2(old_overrides, package / OVERRIDES_FILE)
                carried = True
            except OSError as exc:
                log.warning("could not carry over overrides.json: %s", exc)
        aside = job_dir / f"replaced-{attempt}"
        try:
            os.rename(target, aside)
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise WorkerError(
                ErrorCode.CACHE_PUBLISH_FAILED,
                "The existing cache entry could not be replaced (is it open in the app?).",
                {"target": str(target), "error": str(exc)},
            ) from exc
        faults.crash_point("crash-after-aside")
        try:
            os.rename(package, target)
        except FileExistsError:
            remove_path(aside)
            continue  # lost a race to another publisher; re-inspect
        remove_path(aside)
        return PublishOutcome(
            "replaced-existing",
            replaced_state=existing.state,
            replaced_reasons=existing.reasons,
            overrides_carried=carried,
        )
    raise WorkerError(
        ErrorCode.CACHE_PUBLISH_FAILED,
        "Publishing the cache entry failed repeatedly (concurrent changes).",
        {"target": str(target)},
    )


# --- job directories ---------------------------------------------------------------------


class JobDir:
    """``jobs/<job_id>/``: status.json, job.log and the unpublished ``package/``."""

    def __init__(self, path: Path, job_id: str) -> None:
        self.path = path
        self.job_id = job_id
        self.status: dict[str, Any] = {}

    @classmethod
    def create(cls, jobs_root: Path, job_id: str) -> JobDir:
        path = jobs_root / job_id
        try:
            path.mkdir()
        except FileExistsError as exc:
            raise WorkerError(
                ErrorCode.JOB_EXISTS,
                f"The job directory {job_id} already exists; use a new job_id.",
                {"path": str(path)},
            ) from exc
        except OSError as exc:
            raise WorkerError(
                ErrorCode.DATA_DIR_UNAVAILABLE, f"Cannot create the job directory: {exc}"
            ) from exc
        job = cls(path, job_id)
        (job.package / "stems").mkdir(parents=True)
        return job

    @property
    def package(self) -> Path:
        return self.path / "package"

    @property
    def log_path(self) -> Path:
        return self.path / "job.log"

    @property
    def status_path(self) -> Path:
        return self.path / "status.json"

    def write_status(self, state: str, **fields: Any) -> None:
        if not self.status:
            self.status = {
                "schema_version": JOB_STATUS_SCHEMA_VERSION,
                "kind": JOB_STATUS_KIND,
                "job_id": self.job_id,
                "pid": os.getpid(),
                "created_at": utc_now(),
            }
        self.status.update(fields)
        self.status["state"] = state
        self.status["updated_at"] = utc_now()
        try:
            write_json_atomic(self.status_path, self.status)
        except OSError as exc:
            log.warning("could not write job status: %s", exc)

    def remove_incomplete(self) -> bool:
        """Delete unpublished audio (package/ and replaced-*); keep status.json and job.log."""
        ok = True
        for child in self.path.iterdir() if self.path.exists() else []:
            if child.name == "package" or child.name.startswith("replaced-"):
                remove_path(child)
                ok &= not child.exists()
        return ok

    def remove(self) -> bool:
        shutil.rmtree(self.path, ignore_errors=True)
        return not self.path.exists()


def list_jobs(jobs_root: Path, active_job_id: str | None = None) -> list[dict[str, Any]]:
    """Describe job directories. Only one job runs at a time, so every directory except the
    active job's is abandoned (crashed, killed, or left behind after failure/cancel)."""
    out: list[dict[str, Any]] = []
    if not jobs_root.is_dir():
        return out
    for d in sorted(jobs_root.iterdir()):
        if not d.is_dir():
            continue
        try:
            status = read_json(d / "status.json", max_bytes=1 << 20)
            state = status.get("state") if isinstance(status, dict) else None
        except (OSError, ValueError):
            state = None
        out.append(
            {
                "job_id": d.name,
                "state": state or "unknown",
                "has_package": (d / "package").exists(),
                "abandoned": d.name != active_job_id,
            }
        )
    return out
