"""The pinned model checkpoints: location, verification, and download.

Demucs 4.0.1 defines `htdemucs` as a one-model bag with signature 955717e8; its values were
verified in spikes/htdemucs-artifact. The beat_this `small0` checkpoint (BPM analysis) was
measured in spikes/bpm-library. Checkpoints are only ever fetched by `fetch-model`, never at
job time.
"""

from __future__ import annotations

import hashlib
import http.client
import logging
import os
import ssl
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from snds_worker import __version__
from snds_worker.paths import DataPaths

MODEL_NAME = "htdemucs"
MODEL_SIGNATURE = "955717e8"
MODEL_FILENAME = "955717e8-8726e21a.th"
MODEL_URL = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/955717e8-8726e21a.th"
MODEL_SHA256 = "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"
MODEL_SIZE = 84_141_911

BEAT_MODEL_NAME = "beat_this"
BEAT_MODEL_CHECKPOINT = "small0"
BEAT_MODEL_FILENAME = "small0.ckpt"
BEAT_MODEL_URL = "https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp/small0.ckpt"
BEAT_MODEL_SHA256 = "6074be2c4d490c5f6101fcc374a1ec72ae93456e23bb6019783b849f5dc7d47b"
BEAT_MODEL_SIZE = 8_451_101

_CHUNK_SIZE = 1024 * 1024

log = logging.getLogger(__name__)


class ModelVerificationError(RuntimeError):
    """A downloaded checkpoint did not match the pinned size and SHA-256."""


@dataclass(frozen=True)
class FetchResult:
    path: Path
    downloaded: bool
    size: int
    sha256: str


def model_dir(paths: DataPaths) -> Path:
    """Folder passed to Demucs as its local model repository."""
    return paths.models / MODEL_NAME


def model_path(paths: DataPaths) -> Path:
    return model_dir(paths) / MODEL_FILENAME


def beat_model_dir(paths: DataPaths) -> Path:
    return paths.models / BEAT_MODEL_NAME


def beat_model_path(paths: DataPaths) -> Path:
    return beat_model_dir(paths) / BEAT_MODEL_FILENAME


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        while chunk := file.read(_CHUNK_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def file_matches(path: Path, expected_sha256: str, expected_size: int) -> bool:
    try:
        if path.stat().st_size != expected_size:
            return False
    except FileNotFoundError:
        return False
    return sha256_file(path) == expected_sha256


def fetch(
    destination: Path,
    url: str,
    expected_sha256: str,
    expected_size: int,
    *,
    timeout: float = 60.0,
    attempts: int = 3,
) -> FetchResult:
    """Installs a verified checkpoint, retrying interrupted or truncated downloads."""
    if attempts < 1:
        raise ValueError("attempts must be positive")
    for attempt in range(1, attempts + 1):
        try:
            return _fetch_once(destination, url, expected_sha256, expected_size, timeout=timeout)
        except (
            urllib.error.URLError,
            http.client.HTTPException,
            ssl.SSLError,
            ConnectionError,
            TimeoutError,
        ):
            if attempt == attempts:
                raise
            log.warning("Download attempt %d/%d failed; retrying", attempt, attempts)
            time.sleep(min(2.0 * attempt, 5.0))
    raise AssertionError("unreachable")


def _fetch_once(
    destination: Path,
    url: str,
    expected_sha256: str,
    expected_size: int,
    *,
    timeout: float = 60.0,
) -> FetchResult:
    """Downloads url to destination unless a verified file is already there.

    The download goes to a temporary file in the destination folder and is renamed into place
    only after its size and SHA-256 match, so destination is never left partially written.
    """
    if file_matches(destination, expected_sha256, expected_size):
        log.info("Checkpoint already present and verified: %s", destination)
        return FetchResult(destination, False, expected_size, expected_sha256)

    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(
        prefix=f"{destination.name}.", suffix=".part", dir=destination.parent
    )
    temp_path = Path(temp_name)

    try:
        digest = hashlib.sha256()
        size = 0
        next_report = 10
        request = urllib.request.Request(url, headers={"User-Agent": f"snds-worker/{__version__}"})
        log.info("Downloading %s", url)

        with (
            os.fdopen(fd, "wb") as out,
            urllib.request.urlopen(request, timeout=timeout) as response,
        ):
            announced = int(response.headers.get("Content-Length") or 0)
            if announced and announced != expected_size:
                raise ModelVerificationError(
                    f"Server announced {announced} bytes; expected {expected_size}."
                )

            while chunk := response.read(_CHUNK_SIZE):
                size += len(chunk)
                if size > expected_size:
                    raise ModelVerificationError(
                        f"Download exceeded the expected {expected_size} bytes."
                    )
                out.write(chunk)
                digest.update(chunk)

                percent = size * 100 // expected_size
                if percent >= next_report:
                    log.info("Downloaded %d%% (%d of %d bytes)", percent, size, expected_size)
                    next_report = percent - percent % 10 + 10

            out.flush()
            os.fsync(out.fileno())

        actual_sha256 = digest.hexdigest()
        if size < expected_size:
            raise http.client.IncompleteRead(b"", expected_size - size)
        if size != expected_size or actual_sha256 != expected_sha256:
            raise ModelVerificationError(
                f"Downloaded file has {size} bytes and SHA-256 {actual_sha256}; "
                f"expected {expected_size} bytes and SHA-256 {expected_sha256}."
            )

        os.replace(temp_path, destination)
    except BaseException:
        temp_path.unlink(missing_ok=True)
        raise

    log.info("Checkpoint verified and installed: %s", destination)
    return FetchResult(destination, True, size, actual_sha256)
