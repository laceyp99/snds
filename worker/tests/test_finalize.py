from __future__ import annotations

import dataclasses
import errno
import hashlib
import json
import shutil
from pathlib import Path

import pytest
import soundfile as sf

from helpers import make_request, run_job, write_wav
from snds_worker import bpm, finalize, schema
from snds_worker.cancel import CancelToken
from snds_worker.jobs_protocol import ArtworkInfo

STEMS = ("drums", "bass", "other", "vocals")
STEP_ORDER = [
    "artwork_moved",
    "files_flushed",
    "song_tmp_written",
    "song_json_written",
    "package_verified",
    "published",
]


class Crash(BaseException):
    """Simulates the process dying: nothing after it runs, so nothing is cleaned up."""


@pytest.fixture
def song(tmp_path: Path) -> Path:
    return write_wav(tmp_path / "Ünï 曲.wav", 44_100, 5.0)


def cache_entries(tmp_path: Path) -> list[Path]:
    root = tmp_path / "cache"
    return sorted(root.iterdir()) if root.exists() else []


def assert_cache_is_safe(tmp_path: Path) -> None:
    """Whatever is in the cache is a complete, valid package: nothing partial is visible."""
    for entry in cache_entries(tmp_path):
        status, reason, _ = finalize.verify_package(entry)
        assert status == schema.COMPLETE, f"{entry.name}: {reason}"


def fingerprint(directory: Path) -> dict[str, str]:
    return {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(directory.iterdir())
        if p.is_file()
    }


def publish(tmp_path: Path, song: Path, **kwargs):
    return run_job(tmp_path, song, finalize="default", **kwargs)


def with_artwork(tmp_path: Path, song: Path, name: str = "art.PNG", origin: str = "embedded"):
    request = make_request(tmp_path, song)
    artwork = Path(request.job_dir) / name
    artwork.write_bytes(b"\x89PNG fake image")
    return dataclasses.replace(request, artwork=ArtworkInfo(str(artwork), origin)), artwork


# --- Success ---------------------------------------------------------------------------------


def test_publishes_a_valid_package(tmp_path: Path, song: Path) -> None:
    run = publish(tmp_path, song, analyze_bpm=lambda *_a: bpm.outcome_from_beats(_beats(120.0)))

    assert run.exit_code == 0, run.terminal
    request = run.request
    cache = Path(request.cache_dir)
    assert run.terminal["type"] == "result"
    assert run.terminal["cacheDir"] == request.cache_dir
    assert cache.name == f"{request.source.sha256}-{request.source.size_bytes}"
    assert sorted(p.name for p in cache.iterdir()) == sorted(
        [*(f"{n}.wav" for n in STEMS), "song.json"]
    )
    assert not run.package.exists()  # the folder was renamed, not copied

    data = json.loads((cache / "song.json").read_text(encoding="utf-8"))
    assert schema.classify(data) == (schema.COMPLETE, "")
    assert data["source"]["fileName"] == "Ünï 曲.wav"
    assert data["source"]["path"] == str(song)
    assert data["source"]["sha256"] == request.source.sha256
    assert data["source"]["sizeBytes"] == request.source.size_bytes
    assert data["source"]["container"] == "wav"
    assert data["source"]["frames"] == 5 * 44_100
    for name in STEMS:
        contents = (cache / f"{name}.wav").read_bytes()
        assert data["stems"][name]["sha256"] == hashlib.sha256(contents).hexdigest()
        assert data["stems"][name]["sizeBytes"] == len(contents)
    drums, _ = sf.read(str(cache / "drums.wav"), dtype="float32")
    assert data["stems"]["drums"]["peak"] == pytest.approx(float(abs(drums).max()))
    assert data["bpm"]["value"] == pytest.approx(120.0, abs=0.6)
    assert data["bpm"]["analyzer"]["name"] == "beat_this"
    assert data["artwork"] == {"file": None, "origin": "fallback"}
    assert data["worker"] == {
        "version": "0.0.1",
        "protocol": 1,
        "torch": data["worker"]["torch"],
        "device": "cpu",
    }
    assert data["worker"]["torch"].startswith("2.1.2")
    assert data["preparedAt"].endswith("Z")
    assert finalize.verify_package(cache, cache.name)[0] == schema.COMPLETE

    assert run.terminal["summary"] == {
        "key": cache.name,
        "durationSeconds": 5.0,
        "sampleRate": 44_100,
        "bpm": data["bpm"]["value"],
        "bpmConfidence": data["bpm"]["confidence"],
        "device": "cpu",
        "elapsedSeconds": run.terminal["summary"]["elapsedSeconds"],
    }


def _beats(value: float):
    import numpy as np

    return np.arange(0, 30, 60.0 / value)


def test_failed_bpm_is_published_with_a_null_value(tmp_path: Path, song: Path) -> None:
    run = publish(
        tmp_path, song, analyze_bpm=lambda *_a: bpm.failed("too few beats detected (1)", 1)
    )

    assert run.exit_code == 0, run.terminal
    data = json.loads((Path(run.request.cache_dir) / "song.json").read_text(encoding="utf-8"))
    assert data["bpm"] == {
        "value": None,
        "confidence": 0.0,
        "beats": 1,
        "alternatives": [],
        "error": "too few beats detected (1)",
        "analyzer": data["bpm"]["analyzer"],
        "userOverride": None,
    }
    assert data["bpm"]["analyzer"]["name"] == "beat_this"
    assert schema.classify(data)[0] == schema.COMPLETE
    assert run.terminal["summary"]["bpm"] is None
    assert run.terminal["summary"]["bpmConfidence"] == 0.0


def test_artwork_is_moved_into_the_package(tmp_path: Path, song: Path) -> None:
    request, artwork = with_artwork(tmp_path, song)

    run = publish(tmp_path, song, request=request)

    assert run.exit_code == 0, run.terminal
    cache = Path(request.cache_dir)
    assert (cache / "artwork.png").read_bytes() == b"\x89PNG fake image"
    assert not artwork.exists()
    data = json.loads((cache / "song.json").read_text(encoding="utf-8"))
    assert data["artwork"] == {"file": "artwork.png", "origin": "embedded"}


def test_a_missing_or_odd_artwork_file_falls_back(tmp_path: Path, song: Path) -> None:
    request, artwork = with_artwork(tmp_path, song, origin="file")
    artwork.unlink()

    run = publish(tmp_path, song, request=request)

    assert run.exit_code == 0, run.terminal
    data = json.loads((Path(request.cache_dir) / "song.json").read_text(encoding="utf-8"))
    assert data["artwork"] == {"file": None, "origin": "fallback"}


def test_song_json_is_written_last(tmp_path: Path, song: Path, monkeypatch) -> None:
    seen: dict[str, dict] = {}
    request = make_request(tmp_path, song)
    package = Path(request.job_dir) / "package"

    def watch(name: str) -> None:
        seen[name] = {
            "files": sorted(p.name for p in package.iterdir()) if package.exists() else None,
            "cache_exists": Path(request.cache_dir).exists(),
        }

    monkeypatch.setattr(finalize, "checkpoint", watch)

    run = publish(tmp_path, song, request=request)

    assert run.exit_code == 0, run.terminal
    assert list(seen) == STEP_ORDER
    stems = sorted(f"{n}.wav" for n in STEMS)
    assert seen["files_flushed"]["files"] == stems  # complete audio, no song.json yet
    assert seen["song_tmp_written"]["files"] == sorted([*stems, "song.json.tmp"])
    assert seen["song_json_written"]["files"] == sorted([*stems, "song.json"])
    assert not any(seen[step]["cache_exists"] for step in STEP_ORDER[:-1])
    assert seen["published"]["cache_exists"]


# --- Crash safety ----------------------------------------------------------------------------


@pytest.mark.parametrize("step", STEP_ORDER)
def test_a_crash_at_any_step_never_leaves_a_valid_looking_partial_entry(
    tmp_path: Path, song: Path, monkeypatch, step: str
) -> None:
    request = make_request(tmp_path, song)

    def die(name: str) -> None:
        if name == step:
            raise Crash(step)

    monkeypatch.setattr(finalize, "checkpoint", die)

    with pytest.raises(Crash):
        publish(tmp_path, song, request=request)

    assert_cache_is_safe(tmp_path)
    if step == "published":
        assert Path(request.cache_dir).is_dir()  # renamed, then died before `result`
    else:
        assert not Path(request.cache_dir).exists()
        assert cache_entries(tmp_path) == []
        assert (Path(request.job_dir) / "package").is_dir()  # only jobs\<id> holds the work


def test_a_crash_in_the_middle_of_writing_song_json(
    tmp_path: Path, song: Path, monkeypatch
) -> None:
    request = make_request(tmp_path, song)

    def torn_write(obj, file, **kwargs):
        file.write(json.dumps(obj, **kwargs)[:100])  # half a document
        file.flush()
        raise Crash("power cut")

    monkeypatch.setattr(finalize.json, "dump", torn_write)

    with pytest.raises(Crash):
        publish(tmp_path, song, request=request)

    package = Path(request.job_dir) / "package"
    assert (package / "song.json.tmp").is_file()
    assert not (package / "song.json").exists()
    assert not Path(request.cache_dir).exists()
    assert_cache_is_safe(tmp_path)
    assert finalize.verify_package(package)[0] == schema.CORRUPT  # and never playable


def test_a_torn_song_json_is_not_playable(tmp_path: Path) -> None:
    entry = tmp_path / "cache" / "k"
    entry.mkdir(parents=True)
    (entry / "song.json").write_text('{"cacheSchema": 1, "status": "compl', encoding="utf-8")

    assert finalize.verify_package(entry)[0] == schema.CORRUPT


# --- Errors during finalization --------------------------------------------------------------


def failure_of(run, code: str) -> dict:
    assert run.terminal["type"] == "failure", run.terminal
    assert run.terminal["code"] == code, run.terminal
    assert run.exit_code == 13
    assert not run.package.exists()
    assert not Path(run.request.cache_dir).exists()
    assert_cache_is_safe(Path(run.request.cache_dir).parent.parent)
    return run.terminal


def test_disk_full_while_writing_song_json(tmp_path: Path, song: Path, monkeypatch) -> None:
    def full(*_args):
        raise OSError(errno.ENOSPC, "No space left on device")

    monkeypatch.setattr(finalize, "_write_song_json", full)

    failure = failure_of(publish(tmp_path, song), "disk_full")
    assert failure["retryable"] is True
    assert failure["suggestedAction"] == "free_disk_space"


def test_artwork_that_cannot_be_moved(tmp_path: Path, song: Path, monkeypatch) -> None:
    request, _ = with_artwork(tmp_path, song)

    def locked(*_args):
        raise PermissionError(errno.EACCES, "Access is denied")

    monkeypatch.setattr(finalize, "_move_artwork", locked)

    failure_of(publish(tmp_path, song, request=request), "finalize_failed")


def test_a_rename_that_keeps_failing(tmp_path: Path, song: Path, monkeypatch) -> None:
    calls = []

    def locked(*_args):
        calls.append(1)
        raise PermissionError(errno.EACCES, "Access is denied")

    monkeypatch.setattr(finalize, "_rename", locked)
    monkeypatch.setattr(finalize, "_RENAME_DELAY_SECONDS", 0)

    failure = failure_of(publish(tmp_path, song), "finalize_failed")

    assert len(calls) == finalize._RENAME_ATTEMPTS
    assert failure["retryable"] is True
    assert failure["suggestedAction"] == "retry"


def test_a_transient_rename_failure_is_retried(tmp_path: Path, song: Path, monkeypatch) -> None:
    real = finalize._rename
    attempts = []

    def flaky(source, destination):
        attempts.append(1)
        if len(attempts) < 3:
            raise PermissionError(errno.EACCES, "Access is denied")  # a scanner holds the folder
        real(source, destination)

    monkeypatch.setattr(finalize, "_rename", flaky)
    monkeypatch.setattr(finalize, "_RENAME_DELAY_SECONDS", 0)

    run = publish(tmp_path, song)

    assert run.exit_code == 0, run.terminal
    assert len(attempts) == 3
    assert_cache_is_safe(tmp_path)


def test_inconsistent_metadata_is_never_published(tmp_path: Path, song: Path, monkeypatch) -> None:
    real = finalize.build_metadata

    def broken(job, artwork):
        data = real(job, artwork)
        data["stems"]["drums"]["channels"] = 1
        return data

    monkeypatch.setattr(finalize, "build_metadata", broken)

    failure = failure_of(publish(tmp_path, song), "finalize_failed")
    assert "stems.drums" in failure["details"] or "drums" in failure["details"]


@pytest.mark.parametrize("step", ["artwork_moved", "package_verified"])
def test_cancel_before_publication_wins(tmp_path: Path, song: Path, monkeypatch, step: str) -> None:
    token = CancelToken()

    def cancel_at(name: str) -> None:
        if name == step:
            token.trigger("requested")

    monkeypatch.setattr(finalize, "checkpoint", cancel_at)

    run = publish(tmp_path, song, token=token)

    assert run.terminal["type"] == "cancelled"
    assert run.exit_code == 12
    assert not run.package.exists()
    assert cache_entries(tmp_path) == []


def test_a_cancel_after_publication_cannot_undo_it(tmp_path: Path, song: Path, monkeypatch) -> None:
    token = CancelToken()

    def cancel_at(name: str) -> None:
        if name == "published":
            token.trigger("requested")

    monkeypatch.setattr(finalize, "checkpoint", cancel_at)

    run = publish(tmp_path, song, token=token)

    assert run.terminal["type"] == "result"  # section 3.3 rule 7: the app uses the package
    assert_cache_is_safe(tmp_path)


# --- An existing cache entry -----------------------------------------------------------------


def first_publication(tmp_path: Path, song: Path, value: float = 100.0):
    run = publish(tmp_path, song, analyze_bpm=lambda *_a: bpm.outcome_from_beats(_beats(value)))
    assert run.exit_code == 0, run.terminal
    return run


def test_an_existing_valid_entry_is_reported_and_ours_is_discarded(
    tmp_path: Path, song: Path
) -> None:
    first = first_publication(tmp_path, song, 100.0)
    cache = Path(first.request.cache_dir)
    before = fingerprint(cache)

    second = publish(
        tmp_path,
        song,
        analyze_bpm=lambda *_a: bpm.outcome_from_beats(_beats(90.0)),
        request=first.request,
    )

    assert second.exit_code == 0, second.terminal
    assert second.terminal["type"] == "result"
    assert second.terminal["cacheDir"] == str(cache)
    assert second.terminal["summary"]["bpm"] == first.terminal["summary"]["bpm"]  # theirs, not ours
    assert fingerprint(cache) == before  # untouched
    assert not second.package.exists()  # our duplicate is gone
    assert_cache_is_safe(tmp_path)


@pytest.mark.parametrize(
    "damage",
    [
        "empty folder",
        "no song.json",
        "garbage song.json",
        "other schema",
        "not complete",
        "other model",
        "other protocol",
        "other source",
        "missing stem",
        "stem is not a file",
    ],
)
def test_an_existing_unusable_entry_fails_with_a_stable_code(
    tmp_path: Path, song: Path, damage: str
) -> None:
    first = first_publication(tmp_path, song)
    cache = Path(first.request.cache_dir)
    request = first.request

    def edit(change) -> None:
        data = json.loads((cache / "song.json").read_text(encoding="utf-8"))
        change(data)
        (cache / "song.json").write_text(json.dumps(data), encoding="utf-8")

    if damage == "empty folder":
        shutil.rmtree(cache)
        cache.mkdir()
    elif damage == "no song.json":
        (cache / "song.json").unlink()
    elif damage == "garbage song.json":
        (cache / "song.json").write_text("{not json", encoding="utf-8")
    elif damage == "other schema":
        edit(lambda d: d.update(cacheSchema=2))
    elif damage == "not complete":
        edit(lambda d: d.update(status="preparing"))
    elif damage == "other model":
        edit(lambda d: d["model"].update(signature="deadbeef"))
    elif damage == "other protocol":
        edit(lambda d: d["worker"].update(protocol=2))
    elif damage == "other source":
        edit(lambda d: d["source"].update(sha256="cd" * 32))
    elif damage == "missing stem":
        (cache / "bass.wav").unlink()
    elif damage == "stem is not a file":
        (cache / "bass.wav").unlink()
        (cache / "bass.wav").mkdir()

    snapshot = fingerprint(Path(request.cache_dir))
    run = publish(tmp_path, song, request=request)

    assert run.terminal["type"] == "failure", run.terminal
    assert run.terminal["code"] == "cache_conflict"
    assert run.exit_code == 13
    assert not run.package.exists()
    assert fingerprint(Path(request.cache_dir)) == snapshot  # the app decides what to do with it
    assert str(request.cache_dir) in run.terminal["details"]


def test_a_publication_race_is_resolved_like_an_existing_entry(
    tmp_path: Path, song: Path, monkeypatch
) -> None:
    other = first_publication(tmp_path, song, 100.0)
    cache = Path(other.request.cache_dir)
    template = tmp_path / "template"
    shutil.copytree(cache, template)
    shutil.rmtree(cache)  # nothing there when this job started...

    def raced_rename(_source, destination):
        # ...but another worker published first, so our rename finds the folder.
        shutil.copytree(template, destination)
        raise FileExistsError(183, "Cannot create a file when that file already exists")

    monkeypatch.setattr(finalize, "_rename", raced_rename)

    run = publish(tmp_path, song, request=other.request)

    assert run.terminal["type"] == "result", run.terminal
    assert run.terminal["summary"]["bpm"] == other.terminal["summary"]["bpm"]
    assert_cache_is_safe(tmp_path)
    assert not run.package.exists()
