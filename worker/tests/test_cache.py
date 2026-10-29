"""Cache entry inspection (what the app validates before playback) and atomic publication."""

from __future__ import annotations

import io
import json
import os
import shutil
from pathlib import Path

import pytest

from audio_fixtures import synth_song, write_wav
from snds_worker.cache import (
    CACHE_KEY_RE,
    MANIFEST_FILE,
    OVERRIDES_FILE,
    PIPELINE_ID,
    TEST_PIPELINE_ID,
    JobDir,
    cache_key,
    inspect_entry,
    list_jobs,
    publish_package,
    write_json_atomic,
)
from snds_worker.client import make_request
from snds_worker.control import CancelToken
from snds_worker.job import SeparateJob
from snds_worker.protocol import Emitter, ErrorCode, WorkerError, parse_job_request


@pytest.fixture(scope="module")
def built(tmp_path_factory: pytest.TempPathFactory) -> tuple[Path, str]:
    """One real fake-separator job run in-process; returns (entry dir, key)."""
    base = tmp_path_factory.mktemp("built")
    src = write_wav(base / "song.wav", synth_song(3.0, sr=48000, seed=2), 48000, "PCM_24")
    root = base / "data"
    req = parse_job_request(json.dumps(make_request(src, "built1", "auto", root)))
    old = os.environ.get("SNDS_WORKER_TEST_SEPARATOR")
    os.environ["SNDS_WORKER_TEST_SEPARATOR"] = "fake"
    try:
        out = io.StringIO()
        code = SeparateJob(req, Emitter(out, "separate"), CancelToken()).run()
    finally:
        if old is None:
            os.environ.pop("SNDS_WORKER_TEST_SEPARATOR", None)
        else:
            os.environ["SNDS_WORKER_TEST_SEPARATOR"] = old
    assert code == 0, out.getvalue()
    key = cache_key(req.source.sha256, req.source.size_bytes)
    return root / "cache" / key, key


def _copy(built: tuple[Path, str], tmp_path: Path) -> tuple[Path, str]:
    entry, key = built
    dst = tmp_path / "cache" / key
    shutil.copytree(entry, dst)
    return dst, key


def _edit_manifest(entry: Path, fn) -> None:
    m = json.loads((entry / MANIFEST_FILE).read_text())
    fn(m)
    (entry / MANIFEST_FILE).write_text(json.dumps(m))


def test_cache_key_format() -> None:
    key = cache_key("0" * 64, 123)
    assert key == "0" * 64 + "-123" and CACHE_KEY_RE.match(key)
    assert not CACHE_KEY_RE.match("0" * 64 + "-0123")


def test_valid_entry(built, tmp_path) -> None:
    entry, key = _copy(built, tmp_path)
    r = inspect_entry(entry, expected_key=key, pipeline_id=TEST_PIPELINE_ID, deep=True)
    assert r.state == "valid", r.reasons
    # A fake-separator entry is never compatible with the production pipeline.
    assert inspect_entry(entry, pipeline_id=PIPELINE_ID).state == "incompatible"
    assert inspect_entry(tmp_path / "cache" / "nope").state == "missing"


@pytest.mark.parametrize(
    ("mutate", "state"),
    [
        (lambda e: (e / MANIFEST_FILE).unlink(), "invalid"),
        (lambda e: (e / MANIFEST_FILE).write_text("{not json"), "invalid"),
        (lambda e: (e / MANIFEST_FILE).write_text("[]"), "invalid"),
        (lambda e: _edit_manifest(e, lambda m: m.update(schema_version=2)), "unknown-schema"),
        (lambda e: _edit_manifest(e, lambda m: m.update(schema_version=0)), "invalid"),
        (lambda e: _edit_manifest(e, lambda m: m.update(schema_version="1")), "invalid"),
        (lambda e: _edit_manifest(e, lambda m: m.update(kind="other")), "invalid"),
        (lambda e: _edit_manifest(e, lambda m: m.update(cache_key="f" * 64 + "-1")), "invalid"),
        (lambda e: _edit_manifest(e, lambda m: m["validation"].update(status="failed")), "invalid"),
        (lambda e: _edit_manifest(e, lambda m: m.pop("stems")), "invalid"),
        (
            lambda e: _edit_manifest(e, lambda m: m["pipeline"].update(pipeline_id="old")),
            "incompatible",
        ),
        (lambda e: (e / "stems" / "bass.wav").unlink(), "invalid"),
        (lambda e: (e / "stems" / "drums.wav").open("ab").write(b"\0" * 8), "invalid"),
        (lambda e: (e / "stems" / "vocals.wav").write_bytes(b"x" * 16), "invalid"),
    ],
)
def test_invalid_and_incompatible_entries(built, tmp_path, mutate, state) -> None:
    entry, key = _copy(built, tmp_path)
    mutate(entry)
    r = inspect_entry(entry, expected_key=key, pipeline_id=TEST_PIPELINE_ID)
    assert r.state == state, r.reasons
    assert not r.valid


def test_corrupt_stem_content_needs_a_deep_check(built, tmp_path) -> None:
    entry, key = _copy(built, tmp_path)
    p = entry / "stems" / "other.wav"
    data = bytearray(p.read_bytes())
    data[-100] ^= 0xFF  # same size, different sample
    p.write_bytes(bytes(data))
    assert inspect_entry(entry, pipeline_id=TEST_PIPELINE_ID).valid
    deep = inspect_entry(entry, pipeline_id=TEST_PIPELINE_ID, deep=True)
    assert deep.state == "invalid" and "hash mismatch" in deep.reasons[0]


def test_wrong_directory_name(built, tmp_path) -> None:
    entry, key = _copy(built, tmp_path)
    moved = entry.with_name("0" * 64 + "-1")
    entry.rename(moved)
    assert inspect_entry(moved, pipeline_id=TEST_PIPELINE_ID).state == "invalid"


# --- publication ---------------------------------------------------------------------------


def _package(built, tmp_path: Path, name: str = "job") -> tuple[Path, Path, Path, str]:
    """(package, cache root, job dir, key): a finished job directory ready to publish."""
    entry, key = built
    job_dir = tmp_path / "jobs" / name
    job_dir.mkdir(parents=True)
    shutil.copytree(entry, job_dir / "package")
    cache_root = tmp_path / "cache"
    cache_root.mkdir(exist_ok=True)
    return job_dir / "package", cache_root, job_dir, key


def test_publish_into_empty_cache(built, tmp_path) -> None:
    package, cache_root, job_dir, key = _package(built, tmp_path)
    out = publish_package(package, cache_root, key, job_dir, pipeline_id=TEST_PIPELINE_ID)
    assert out.outcome == "published"
    assert not package.exists()
    assert inspect_entry(cache_root / key, pipeline_id=TEST_PIPELINE_ID, deep=True).valid


def test_publish_keeps_an_existing_valid_entry(built, tmp_path) -> None:
    package, cache_root, job_dir, key = _package(built, tmp_path)
    shutil.copytree(built[0], cache_root / key)
    marker = cache_root / key / OVERRIDES_FILE
    marker.write_text('{"schema_version": 1}')
    out = publish_package(package, cache_root, key, job_dir, pipeline_id=TEST_PIPELINE_ID)
    assert out.outcome == "reused-existing"
    assert marker.exists() and not package.exists()


@pytest.mark.parametrize("damage", ["schema", "missing-stem", "pipeline", "no-manifest", "file"])
def test_publish_replaces_invalid_entries_and_carries_overrides(built, tmp_path, damage) -> None:
    package, cache_root, job_dir, key = _package(built, tmp_path)
    target = cache_root / key
    if damage == "file":
        target.write_bytes(b"garbage")
    else:
        shutil.copytree(built[0], target)
        (target / OVERRIDES_FILE).write_text('{"schema_version": 1, "bpm": {"value": 64.0}}')
        if damage == "schema":
            _edit_manifest(target, lambda m: m.update(schema_version=99))
        elif damage == "missing-stem":
            (target / "stems" / "vocals.wav").unlink()
        elif damage == "pipeline":
            _edit_manifest(target, lambda m: m["pipeline"].update(pipeline_id="snds-p0-old"))
        else:
            (target / MANIFEST_FILE).unlink()
    out = publish_package(package, cache_root, key, job_dir, pipeline_id=TEST_PIPELINE_ID)
    assert out.outcome == "replaced-existing"
    assert inspect_entry(target, pipeline_id=TEST_PIPELINE_ID, deep=True).valid
    assert not any(p.name.startswith("replaced-") for p in job_dir.iterdir())
    if damage != "file":
        assert out.overrides_carried
        assert json.loads((target / OVERRIDES_FILE).read_text())["bpm"]["value"] == 64.0


def test_publish_blocked_by_an_open_file_in_the_old_entry(built, tmp_path) -> None:
    package, cache_root, job_dir, key = _package(built, tmp_path)
    target = cache_root / key
    shutil.copytree(built[0], target)
    (target / "stems" / "vocals.wav").unlink()
    with (target / "stems" / "drums.wav").open("rb"), pytest.raises(WorkerError) as ei:
        publish_package(package, cache_root, key, job_dir, pipeline_id=TEST_PIPELINE_ID)
    assert ei.value.code == ErrorCode.CACHE_PUBLISH_FAILED
    assert package.exists()  # nothing was lost; the job cleans it up


def test_write_json_atomic_leaves_no_temp_files(tmp_path) -> None:
    target = tmp_path / "x.json"
    write_json_atomic(target, {"a": 1})
    write_json_atomic(target, {"a": 2})
    assert json.loads(target.read_text()) == {"a": 2}
    assert [p.name for p in tmp_path.iterdir()] == ["x.json"]
    with pytest.raises(ValueError):
        write_json_atomic(target, {"a": float("nan")})
    assert json.loads(target.read_text()) == {"a": 2}
    assert [p.name for p in tmp_path.iterdir()] == ["x.json"]


def test_job_dirs_and_abandoned_detection(tmp_path) -> None:
    jobs = tmp_path / "jobs"
    jobs.mkdir()
    a = JobDir.create(jobs, "a")
    a.write_status("running", stage="separating")
    b = JobDir.create(jobs, "b")
    b.write_status("cancelled")
    b.remove_incomplete()
    (jobs / "c").mkdir()  # crashed before writing status
    with pytest.raises(WorkerError) as ei:
        JobDir.create(jobs, "a")
    assert ei.value.code == ErrorCode.JOB_EXISTS
    listed = {j["job_id"]: j for j in list_jobs(jobs, active_job_id="a")}
    assert listed["a"] == {
        "job_id": "a",
        "state": "running",
        "has_package": True,
        "abandoned": False,
    }
    assert listed["b"]["state"] == "cancelled" and not listed["b"]["has_package"]
    assert (
        listed["b"]["abandoned"] and listed["c"]["abandoned"] and listed["c"]["state"] == "unknown"
    )
    status = json.loads((jobs / "a" / "status.json").read_text())
    assert status["schema_version"] == 1 and status["kind"] == "snds-job-status"
    assert status["pid"] == os.getpid()
