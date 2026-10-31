"""Atomic publication of a song package (docs/protocol.md section 5.3).

The package is built in `jobDir\\package`, which never contains `song.json` until every other
file is complete and validated. `song.json` is written last (to a temporary name, then renamed),
the package is verified once more, and then the whole folder is renamed to `cacheDir`. A rename
within one volume is atomic, so `cacheDir` either does not exist or is a complete package: a
crash at any point leaves only `jobs\\<jobId>\\`.
"""

from __future__ import annotations

import errno
import hashlib
import json
import logging
import os
import shutil
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Any

from snds_worker import schema
from snds_worker.cancel import CancelToken
from snds_worker.errors import JobFailure
from snds_worker.protocol import PROTOCOL_VERSION

if TYPE_CHECKING:
    from snds_worker.pipeline import PreparedJob

log = logging.getLogger(__name__)

SONG_TEMP = "song.json.tmp"
_RENAME_ATTEMPTS = 6
_RENAME_DELAY_SECONDS = 0.15
_SAFE_EXTENSION = frozenset("abcdefghijklmnopqrstuvwxyz0123456789")


def checkpoint(_name: str) -> None:
    """A named boundary between finalization steps; tests raise here to simulate a crash."""


def _fail(message: str, details: str | None = None, **kwargs: Any) -> JobFailure:
    return JobFailure("finalize_failed", message, details=details, **kwargs)


def _io_failure(action: str, error: OSError) -> JobFailure:
    if error.errno == errno.ENOSPC:
        return JobFailure(
            "disk_full",
            "There is not enough free disk space to save the song.",
            details=f"{action}: {error}",
            retryable=True,
            suggested_action="free_disk_space",
        )
    return _fail(f"Could not save the song ({action}: {error.strerror or error}).", str(error))


def _fsync(path: Path) -> None:
    with path.open("rb+") as file:
        os.fsync(file.fileno())


def _move_artwork(job: PreparedJob) -> dict[str, Any]:
    """Moves the app's artwork image into the package as artwork.<ext>."""
    artwork = job.request.artwork
    if artwork is None:
        return {"file": None, "origin": "fallback"}

    source = Path(artwork.path)
    extension = source.suffix.lstrip(".").lower()
    if not source.is_file() or not extension or not set(extension) <= _SAFE_EXTENSION:
        log.warning("Artwork %s is missing or has no usable extension; using the fallback", source)
        return {"file": None, "origin": "fallback"}

    name = f"artwork.{extension}"
    os.replace(source, job.package_dir / name)  # same volume: both are under jobDir
    return {"file": name, "origin": artwork.origin}


def build_metadata(job: PreparedJob, artwork: dict[str, Any]) -> dict[str, Any]:
    request = job.request
    source = job.source
    stems = {}
    for name, stem in job.stems.items():
        entry = schema.stem_entry(stem.frames, stem.sample_rate, stem.peak, name)
        with stem.path.open("rb") as file:
            entry["sha256"] = hashlib.file_digest(file, "sha256").hexdigest()
            entry["sizeBytes"] = os.fstat(file.fileno()).st_size
        stems[name] = entry
    try:
        import torch

        torch_version = str(torch.__version__)
    except ImportError:  # pragma: no cover - the worker cannot run without torch
        torch_version = "unknown"

    return schema.build_song_metadata(
        source={
            "fileName": Path(request.source.path).name,
            "path": request.source.path,
            "sizeBytes": request.source.size_bytes,
            "sha256": request.source.sha256,
            "container": source.container,
            "sampleRate": source.sample_rate,
            "channels": source.channels,
            "frames": source.frames,
            "durationSeconds": source.duration_seconds,
        },
        stems=stems,
        bpm=job.bpm.to_json() if job.bpm is not None else _no_bpm(),
        worker={
            "version": _worker_version(),
            "protocol": PROTOCOL_VERSION,
            "torch": torch_version,
            "device": job.device,
        },
        prepared_at=datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        artwork=artwork,
    )


def _worker_version() -> str:
    from snds_worker import __version__

    return __version__


def _no_bpm() -> dict[str, Any]:
    from snds_worker import bpm

    return bpm.failed("BPM analysis did not run").to_json()


def _write_song_json(package: Path, metadata: dict[str, Any]) -> None:
    """Writes song.json via a temporary name in the same folder, flushed before the rename."""
    temp = package / SONG_TEMP
    with temp.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(metadata, file, indent=2, ensure_ascii=False, allow_nan=False)
        file.write("\n")
        file.flush()
        os.fsync(file.fileno())
    checkpoint("song_tmp_written")
    os.replace(temp, package / schema.SONG_FILE)


def verify_package(
    directory: Path, key: str | None = None
) -> tuple[str, str, dict[str, Any] | None]:
    """Classifies a package folder as the app would. Returns (status, reason, song.json data).

    `status` is schema.COMPLETE only if song.json parses and classifies as complete, the folder
    name matches the cache key recomputed from the source (when `key` is given, it is the
    expected key), and every stem and artwork file exists.
    """
    try:
        data = json.loads((directory / schema.SONG_FILE).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return schema.CORRUPT, f"song.json is missing or unreadable ({error})", None

    status, reason = schema.classify(data)
    if status != schema.COMPLETE:
        return status, reason, data

    source = data["source"]
    expected = schema.cache_key(source["sha256"], source["sizeBytes"])
    if key is not None and expected != key:
        return schema.CORRUPT, f"song.json describes {expected}, not {key}", data

    names = [stem["file"] for stem in data["stems"].values()]
    artwork = data["artwork"].get("file")
    if artwork:
        names.append(artwork)
    missing = [name for name in names if not (directory / name).is_file()]
    if missing:
        return schema.CORRUPT, f"files are missing: {', '.join(missing)}", data
    return schema.COMPLETE, "", data


def summary_from(data: dict[str, Any], key: str, elapsed_seconds: float) -> dict[str, Any]:
    bpm = data["bpm"]
    return {
        "key": key,
        "durationSeconds": data["source"]["durationSeconds"],
        "sampleRate": data["source"]["sampleRate"],
        "bpm": bpm.get("value"),
        "bpmConfidence": bpm.get("confidence", 0),
        "device": data["worker"]["device"],
        "elapsedSeconds": round(elapsed_seconds, 1),
    }


def _rename(source: Path, destination: Path) -> None:
    os.rename(source, destination)  # fails if destination exists; never replaces it


def _publish(package: Path, cache_dir: Path) -> bool:
    """Renames the package to cache_dir. Returns False if a cache entry appeared meanwhile."""
    cache_dir.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(_RENAME_ATTEMPTS):
        try:
            _rename(package, cache_dir)
            return True
        except FileExistsError:
            return False
        except OSError as error:
            if cache_dir.exists():
                return False
            if error.errno == errno.ENOSPC:
                raise _io_failure("publishing the package", error) from error
            # Virus scanners and indexers briefly hold new folders open on Windows.
            if attempt == _RENAME_ATTEMPTS - 1:
                raise _fail(
                    f"Could not save the song ({error.strerror or error}). Close programs that "
                    "may be scanning the cache folder and try again.",
                    str(error),
                    retryable=True,
                    suggested_action="retry",
                ) from error
            time.sleep(_RENAME_DELAY_SECONDS)
    return False  # pragma: no cover


def finalize_package(job: PreparedJob, token: CancelToken) -> dict[str, Any]:
    """Finishes the package in jobDir and publishes it to cacheDir; returns the result summary."""
    request = job.request
    package = job.package_dir
    cache_dir = Path(request.cache_dir)
    key = schema.cache_key(request.source.sha256, request.source.size_bytes)

    try:
        token.raise_if_cancelled()
        artwork = _move_artwork(job)
        checkpoint("artwork_moved")

        for stem in job.stems.values():
            _fsync(stem.path)
        if artwork["file"]:
            _fsync(package / artwork["file"])
        checkpoint("files_flushed")

        metadata = build_metadata(job, artwork)
        problems = schema.validate_song_metadata(metadata)
        if problems:
            raise _fail("The song metadata is inconsistent.", "; ".join(problems))
        if (package / schema.SONG_FILE).exists():
            raise _fail("The package already contains song.json.")

        token.raise_if_cancelled()
        _write_song_json(package, metadata)
        checkpoint("song_json_written")

        status, reason, _ = verify_package(package, key)
        if status != schema.COMPLETE:
            raise _fail("The prepared package did not verify.", reason)
        checkpoint("package_verified")

        token.raise_if_cancelled()  # the last point at which a cancel request wins
    except OSError as error:
        raise _io_failure("writing the package", error) from error

    published = _publish(package, cache_dir)
    checkpoint("published")
    elapsed = time.monotonic() - job.started_at

    if published:
        log.info("Published %s", cache_dir)
        return summary_from(metadata, key, elapsed)

    return _adopt_existing(cache_dir, key, package, elapsed)


def _adopt_existing(cache_dir: Path, key: str, package: Path, elapsed: float) -> dict[str, Any]:
    """cacheDir already exists: use it if it is a valid package for this source, else fail."""
    status, reason, data = verify_package(cache_dir, key)
    if status != schema.COMPLETE or data is None:
        raise JobFailure(
            "cache_conflict",
            "A damaged or incompatible cache entry for this song already exists. Delete it "
            "from the cache and prepare the song again.",
            details=f"{cache_dir}: {status}: {reason}",
        )

    log.info("A valid cache entry already exists at %s; discarding the new package", cache_dir)
    shutil.rmtree(package, ignore_errors=True)
    return summary_from(data, key, elapsed)
