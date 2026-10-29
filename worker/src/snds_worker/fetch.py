"""``fetch-model``: install the pinned htdemucs checkpoint (idempotent, SHA-256 verified)."""

from __future__ import annotations

import logging
from typing import Any

from snds_worker.model import (
    CHECKPOINT_SHA256,
    CHECKPOINT_SIZE,
    CHECKPOINT_URL,
    checkpoint_path,
    download_file,
    model_dir,
    verify_checkpoint,
    write_bag_yaml,
    write_memo,
)
from snds_worker.paths import DataDirs
from snds_worker.protocol import Emitter, ErrorCode, WarningCode, WorkerError

log = logging.getLogger(__name__)

STAGES = (
    ("checking-model", "Checking the installed model"),
    ("downloading", "Downloading the model"),
    ("installing", "Installing the model"),
)


def fetch_model(emitter: Emitter, dirs: DataDirs, url: str = CHECKPOINT_URL) -> dict[str, Any]:
    dirs.ensure(model_dir(dirs.models))
    target = checkpoint_path(dirs.models)

    emitter.stage("checking-model", 0, len(STAGES), STAGES[0][1])
    status = verify_checkpoint(dirs.models, use_memo=False)
    downloaded = False
    info: dict[str, Any] = {}
    if status.ok:
        log.info("model already installed and verified: %s", target)
    else:
        if status.present:
            emitter.warning(
                WarningCode.MODEL_REPLACED,
                f"The installed model is invalid ({status.problem}); downloading it again.",
            )
            try:
                target.unlink()
            except OSError as exc:
                raise WorkerError(
                    ErrorCode.MODEL_DOWNLOAD_FAILED, f"Cannot remove the invalid model: {exc}"
                ) from exc
        emitter.stage("downloading", 1, len(STAGES), STAGES[1][1])
        log.info("downloading %s (%.1f MB)", url, CHECKPOINT_SIZE / 1e6)
        last = [-1.0]

        def progress(done: int, total: int) -> None:
            frac = done / total
            if frac - last[0] >= 0.02 or done == total:
                last[0] = frac
                emitter.progress("downloading", frac, frac, {"bytes": done, "bytes_total": total})

        info = download_file(
            url,
            target,
            expected_size=CHECKPOINT_SIZE,
            expected_sha256=CHECKPOINT_SHA256,
            progress=progress,
        )
        downloaded = True
        log.info("downloaded and verified in %d attempt(s)", info["attempts"])

    emitter.stage("installing", 2, len(STAGES), STAGES[2][1])
    yaml_written = write_bag_yaml(dirs.models)
    write_memo(target, target.stat(), CHECKPOINT_SHA256)
    return {
        "model": {
            "path": str(target),
            "size": CHECKPOINT_SIZE,
            "sha256": CHECKPOINT_SHA256,
            "downloaded": downloaded,
            "yaml_written": yaml_written,
            "url": url,
            "http": info.get("http"),
        }
    }
