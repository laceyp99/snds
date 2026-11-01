"""The `song.json` provenance schema (cacheSchema 1) and cache key (see docs/protocol.md).

Pure and dependency-free. `classify` is the single rule for whether a package is playable; the
C++ loader (src/model/SongMetadata.cpp) implements the same rules.
"""

from __future__ import annotations

import re
from collections.abc import Mapping
from typing import Any

from snds_worker import model
from snds_worker.protocol import PROTOCOL_VERSION

CACHE_SCHEMA = 1
SONG_FILE = "song.json"
STEM_NAMES = ("drums", "bass", "other", "vocals")
STATUS_COMPLETE = "complete"
BPM_OVERRIDE_MODES = ("numeric", "half", "double")
ARTWORK_ORIGINS = ("embedded", "file", "fallback")

COMPLETE = "complete"
INCOMPATIBLE = "incompatible"
CORRUPT = "corrupt"

_SHA256 = re.compile(r"[0-9a-f]{64}")
_UTC_TIMESTAMP = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z")


def cache_key(sha256: str, size_bytes: int) -> str:
    return f"{sha256}-{size_bytes}"


def stem_file_name(stem: str) -> str:
    return f"{stem}.wav"


def build_song_metadata(
    *,
    source: dict[str, Any],
    stems: dict[str, dict[str, Any]],
    bpm: dict[str, Any],
    worker: dict[str, Any],
    prepared_at: str,
    artwork: dict[str, Any] | None = None,
    model_info: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Assembles the document. Callers pass the measured parts; fixed facts are filled in."""
    return {
        "cacheSchema": CACHE_SCHEMA,
        "status": STATUS_COMPLETE,
        "source": source,
        "stems": stems,
        "model": model_info
        or {
            "name": model.MODEL_NAME,
            "signature": model.MODEL_SIGNATURE,
            "file": model.MODEL_FILENAME,
            "sha256": model.MODEL_SHA256,
        },
        "bpm": bpm,
        "artwork": artwork or {"file": None, "origin": "fallback"},
        "worker": worker,
        "preparedAt": prepared_at,
    }


def stem_entry(frames: int, sample_rate: int, peak: float, stem: str) -> dict[str, Any]:
    return {
        "file": stem_file_name(stem),
        "frames": frames,
        "sampleRate": sample_rate,
        "channels": 2,
        "sampleFormat": "float32",
        "peak": peak,
    }


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and value == value


def _is_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _is_sha(value: Any) -> bool:
    return isinstance(value, str) and _SHA256.fullmatch(value) is not None


def _structure_problems(data: Mapping[str, Any]) -> list[str]:
    problems: list[str] = []

    def need(condition: bool, text: str) -> None:
        if not condition:
            problems.append(text)

    def section(key: str) -> Mapping[str, Any]:
        value = data.get(key)
        if isinstance(value, dict):
            return value
        problems.append(f"{key} is missing or not an object")
        return {}

    source = section("source")
    for key in ("fileName", "path", "container"):
        need(_is_string(source.get(key)), f"source.{key} must be a non-empty string")
    need(_is_int(source.get("sizeBytes")) and source["sizeBytes"] >= 0, "source.sizeBytes invalid")
    need(_is_sha(source.get("sha256")), "source.sha256 must be 64 lowercase hex characters")
    for key in ("sampleRate", "channels", "frames"):
        need(_is_int(source.get(key)) and source[key] > 0, f"source.{key} must be positive")
    duration = source.get("durationSeconds")
    need(_is_number(duration) and duration > 0, "source.durationSeconds must be positive")

    stems = section("stems")
    for name in STEM_NAMES:
        entry = stems.get(name)
        if not isinstance(entry, dict):
            problems.append(f"stems.{name} is missing")
            continue
        need(entry.get("file") == stem_file_name(name), f"stems.{name}.file must be the stem file")
        for key in ("frames", "sampleRate"):
            need(_is_int(entry.get(key)) and entry[key] > 0, f"stems.{name}.{key} must be positive")
        need(entry.get("channels") == 2, f"stems.{name}.channels must be 2")
        need(entry.get("sampleFormat") == "float32", f"stems.{name}.sampleFormat must be float32")
        peak = entry.get("peak")
        need(_is_number(peak) and peak >= 0 and peak != float("inf"), f"stems.{name}.peak invalid")
        # Older schema-1 packages omit integrity metadata; new entries supply both fields.
        if "sha256" in entry or "sizeBytes" in entry:
            need(
                _is_sha(entry.get("sha256")),
                f"stems.{name}.sha256 must be 64 lowercase hex characters",
            )
            need(
                _is_int(entry.get("sizeBytes")) and entry["sizeBytes"] > 0,
                f"stems.{name}.sizeBytes must be positive",
            )
        if _is_int(source.get("frames")):
            need(entry.get("frames") == source["frames"], f"stems.{name}.frames != source.frames")
        if _is_int(source.get("sampleRate")):
            need(
                entry.get("sampleRate") == source["sampleRate"],
                f"stems.{name}.sampleRate != source.sampleRate",
            )

    mdl = section("model")
    for key in ("name", "signature", "file"):
        need(_is_string(mdl.get(key)), f"model.{key} must be a non-empty string")
    need(_is_sha(mdl.get("sha256")), "model.sha256 must be 64 lowercase hex characters")

    bpm = section("bpm")
    value = bpm.get("value")
    error = bpm.get("error")
    need(error is None or _is_string(error), "bpm.error must be a non-empty string or null")
    if value is None:
        # Failed detection does not fail the job: the package says so instead.
        need(_is_string(error), "bpm.error is required when bpm.value is null")
    else:
        need(_is_number(value) and value > 0, "bpm.value must be positive or null")
        need(error is None, "bpm.error must be null when bpm.value is set")
    confidence = bpm.get("confidence")
    need(_is_number(confidence) and 0 <= confidence <= 1, "bpm.confidence must be within 0..1")
    need(_is_int(bpm.get("beats")) and bpm["beats"] >= 0, "bpm.beats must be non-negative")
    alternatives = bpm.get("alternatives")
    need(
        isinstance(alternatives, list) and all(_is_number(a) and a > 0 for a in alternatives),
        "bpm.alternatives must be a list of positive numbers",
    )
    analyzer = bpm.get("analyzer")
    if isinstance(analyzer, dict):
        need(_is_string(analyzer.get("name")), "bpm.analyzer.name must be a string")
        need(_is_string(analyzer.get("version")), "bpm.analyzer.version must be a string")
        need(_is_sha(analyzer.get("checkpointSha256")), "bpm.analyzer.checkpointSha256 invalid")
    else:
        problems.append("bpm.analyzer is missing")
    override = bpm.get("userOverride")
    if override is not None:
        if isinstance(override, dict):
            need(
                _is_number(override.get("value")) and override["value"] > 0,
                "bpm.userOverride.value must be positive",
            )
            need(
                override.get("mode") in BPM_OVERRIDE_MODES,
                "bpm.userOverride.mode must be numeric, half or double",
            )
        else:
            problems.append("bpm.userOverride must be an object or null")

    artwork = section("artwork")
    file = artwork.get("file")
    need(file is None or _is_string(file), "artwork.file must be a string or null")
    need(
        not isinstance(file, str) or not any(c in file for c in "/\\:"),
        "artwork.file must be a plain file name",
    )
    need(artwork.get("origin") in ARTWORK_ORIGINS, "artwork.origin invalid")

    worker = section("worker")
    for key in ("version", "torch", "device"):
        need(_is_string(worker.get(key)), f"worker.{key} must be a non-empty string")
    need(_is_int(worker.get("protocol")), "worker.protocol must be an integer")

    need(
        isinstance(data.get("preparedAt"), str)
        and _UTC_TIMESTAMP.fullmatch(data["preparedAt"]) is not None,
        "preparedAt must be a UTC ISO-8601 timestamp ending in Z",
    )
    return problems


def classify(data: Any) -> tuple[str, str]:
    """Returns (COMPLETE | INCOMPATIBLE | CORRUPT, reason). Reason is empty when complete.

    Order matters: the schema number decides first, because a different schema's layout cannot
    be validated; then structure; then compatibility with this build's model and protocol.
    """
    if not isinstance(data, dict):
        return CORRUPT, "song.json is not a JSON object"

    schema = data.get("cacheSchema")
    if not _is_int(schema):
        return CORRUPT, "cacheSchema is missing or not an integer"
    if schema != CACHE_SCHEMA:
        return INCOMPATIBLE, f"cacheSchema {schema} is not supported (expected {CACHE_SCHEMA})"

    if data.get("status") != STATUS_COMPLETE:
        return CORRUPT, f"status is {data.get('status')!r}, not 'complete'"

    problems = _structure_problems(data)
    if problems:
        return CORRUPT, problems[0]

    mdl = data["model"]
    if mdl["name"] != model.MODEL_NAME or mdl["signature"] != model.MODEL_SIGNATURE:
        return INCOMPATIBLE, f"prepared with model {mdl['name']} {mdl['signature']}"
    if data["worker"]["protocol"] != PROTOCOL_VERSION:
        return INCOMPATIBLE, f"prepared with protocol {data['worker']['protocol']}"
    return COMPLETE, ""


def validate_song_metadata(data: Any) -> list[str]:
    """Empty list when the document is a playable, compatible package; otherwise the reason."""
    status, reason = classify(data)
    return [] if status == COMPLETE else [reason]
