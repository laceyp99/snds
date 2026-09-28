"""Spike: acquire the pinned htdemucs checkpoint into the app-controlled models dir.

    uv run --project worker --no-sync python spikes/python/fetch_htdemucs.py

Layout produced (never inside the repo, never in the torch.hub cache):

    %LOCALAPPDATA%\\snds\\models\\htdemucs\\955717e8-8726e21a.th   (pinned size + SHA-256)
    %LOCALAPPDATA%\\snds\\models\\htdemucs\\htdemucs.yaml           ("models: ['955717e8']")

The download streams to a ``.part`` file, is hashed while streaming, and is renamed into
place only when size and SHA-256 match. Re-running is a no-op when the file verifies.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import urllib.request

from _common import (
    HTDEMUCS_BAG_YAML,
    HTDEMUCS_FILE,
    HTDEMUCS_NAME,
    HTDEMUCS_SHA256,
    HTDEMUCS_SIZE,
    HTDEMUCS_URL,
    Timer,
    models_dir,
    sha256_file,
)


def main() -> int:
    root = models_dir()
    root.mkdir(parents=True, exist_ok=True)
    target = root / HTDEMUCS_FILE
    result: dict[str, object] = {"url": HTDEMUCS_URL, "path": str(target)}

    if target.exists() and target.stat().st_size == HTDEMUCS_SIZE:
        with Timer() as t:
            digest = sha256_file(target)
        result.update(cached=True, sha256=digest, hash_seconds=round(t.seconds, 3))
    else:
        part = target.with_suffix(".th.part")
        h = hashlib.sha256()
        size = 0
        req = urllib.request.Request(HTDEMUCS_URL, headers={"User-Agent": "snds-spike/0.1"})
        with Timer() as t, urllib.request.urlopen(req, timeout=60) as resp, part.open("wb") as f:
            result["http_last_modified"] = resp.headers.get("Last-Modified")
            result["http_etag"] = resp.headers.get("ETag")
            result["s3_version_id"] = resp.headers.get("x-amz-version-id")
            while block := resp.read(1 << 20):
                f.write(block)
                h.update(block)
                size += len(block)
        digest = h.hexdigest()
        result.update(cached=False, download_seconds=round(t.seconds, 2), sha256=digest)
        if size != HTDEMUCS_SIZE or digest != HTDEMUCS_SHA256:
            part.unlink(missing_ok=True)
            result.update(ok=False, size=size, error="size/sha256 mismatch; file discarded")
            print(json.dumps(result, indent=2))
            return 1
        os.replace(part, target)

    (root / f"{HTDEMUCS_NAME}.yaml").write_text(HTDEMUCS_BAG_YAML, encoding="utf-8")
    result.update(
        size=target.stat().st_size,
        sha256_matches_pin=result["sha256"] == HTDEMUCS_SHA256,
        filename_checksum_prefix_ok=str(result["sha256"]).startswith(
            HTDEMUCS_FILE.split("-")[1].split(".")[0]
        ),
        dir_listing=sorted(p.name for p in root.iterdir()),
    )
    result["ok"] = bool(result["sha256_matches_pin"] and result["filename_checksum_prefix_ok"])
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
