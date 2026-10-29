"""End-to-end protocol tests: spawn ``python -m snds_worker separate`` like the app does and
read its NDJSON. Uses the fake separator (no model, no torch)."""

from __future__ import annotations

import json
import shutil
import time
from pathlib import Path

import pytest

from audio_fixtures import synth_song, write_mp3, write_wav
from conftest import FAKE_ENV, load_manifest, new_job_id, separate
from snds_worker.cache import (
    OVERRIDES_FILE,
    TEST_PIPELINE_ID,
    cache_key,
    inspect_entry,
    list_jobs,
)
from snds_worker.client import WorkerProcess, make_request, run_worker
from snds_worker.faults import CRASH_EXIT_CODE
from snds_worker.job import STAGE_IDS
from snds_worker.model import SOURCES


@pytest.fixture
def wav48(tmp_path: Path) -> Path:
    frames = 48000 * 8 + 321
    x = synth_song(frames / 48000 + 0.01, sr=48000, seed=11)[:frames]
    return write_wav(tmp_path / "song48.wav", x, 48000, "PCM_24")


@pytest.fixture
def long_wav(tmp_path: Path) -> Path:
    return write_wav(tmp_path / "long.wav", synth_song(20.0, sr=44100, seed=12), 44100)


def _key(path: Path) -> str:
    req = make_request(path)
    return cache_key(req["source"]["sha256"], req["source"]["size_bytes"])


def _no_entry(root: Path, source: Path) -> bool:
    return not (root / "cache" / _key(source)).exists()


def _env(**extra: str) -> dict[str, str]:
    return {**FAKE_ENV, **extra}


def test_successful_job_message_flow(wav48: Path, data_root: Path) -> None:
    job_id = new_job_id()
    res = separate(wav48, data_root, env=_env(), job_id=job_id)
    assert res.exit_code == 0, res.stderr
    assert res.consistent and not res.protocol_errors
    msgs = res.messages
    assert msgs[0]["type"] == "hello" and msgs[0]["command"] == "separate"
    assert msgs[0]["pipeline_id"] == TEST_PIPELINE_ID
    assert [m["stage"] for m in res.of_type("stage")] == list(STAGE_IDS)
    assert all(m["stage_count"] == len(STAGE_IDS) for m in res.of_type("stage"))
    overall = [m["overall"] for m in res.of_type("progress")]
    assert overall == sorted(overall) and overall[-1] == 1.0
    seg = [
        m["detail"]
        for m in res.of_type("progress")
        if m["stage"] == "separating" and m.get("detail")
    ]
    assert seg[-1]["segments_done"] == seg[-1]["segments_total"] == 9
    device = res.of_type("device")[0]
    assert device["selected"] == "cpu" and device["reason"] == "test-separator"
    assert [m["seq"] for m in msgs] == list(range(1, len(msgs) + 1))

    result = res.terminal
    assert result["type"] == "result" and result["exit_code"] == 0
    assert result["publish"] == "published" and result["job_id"] == job_id
    entry = Path(result["cache_dir"])
    assert entry.name == result["cache_key"] == _key(wav48)
    assert inspect_entry(entry, pipeline_id=TEST_PIPELINE_ID, deep=True).valid
    assert not (data_root / "jobs" / job_id).exists()  # removed on success

    m = load_manifest(entry)
    assert m["schema_version"] == 1 and m["kind"] == "snds-song-package"
    assert m["source"]["samplerate"] == 48000 and m["source"]["decoded_frames"] == 48000 * 8 + 321
    assert m["stems"]["frames"] == m["source"]["decoded_frames"]
    assert m["stems"]["order"] == list(SOURCES)
    assert m["validation"]["status"] == "passed"
    assert m["validation"]["metrics"]["alignment"]["lag_samples"] == 0
    assert m["bpm"]["status"] == "ok" and abs(m["bpm"]["value"] - 120) < 1.5
    assert m["pipeline"]["test_mode"] == {"SNDS_WORKER_TEST_SEPARATOR": "fake"}
    assert m["producer"]["protocol_version"] == 1
    assert set(m["job"]["timings"]) >= set(STAGE_IDS[:-1])
    assert result["bpm"]["value"] == m["bpm"]["value"]


def test_mono_and_mp3_sources(tmp_path: Path, data_root: Path) -> None:
    frames = 44100 * 6 + 5
    mono = write_wav(
        tmp_path / "mono.wav", synth_song(frames / 44100 + 0.01, channels=1)[:frames], 44100
    )
    res = separate(mono, data_root, env=_env())
    assert res.exit_code == 0, res.stderr
    m = load_manifest(Path(res.terminal["cache_dir"]))
    assert m["source"]["channels"] == 1 and m["stems"]["channels"] == 2
    assert m["stems"]["frames"] == frames

    mp3 = write_mp3(tmp_path / "s.mp3", synth_song(frames / 48000 + 0.01, sr=48000)[:frames], 48000)
    res = separate(mp3, data_root, env=_env())
    assert res.exit_code == 0, res.stderr
    m = load_manifest(Path(res.terminal["cache_dir"]))
    assert m["source"]["format"] == "MP3" and m["source"]["decoded_frames"] == frames
    assert m["source"]["mp3"]["gapless_info"] and m["source"]["mp3"]["gapless_frames"] == frames


def test_unknown_protocol_version(wav48: Path, data_root: Path) -> None:
    req = make_request(wav48, new_job_id(), data_root=data_root)
    req["protocol_version"] = 2
    res = separate(wav48, data_root, env=_env(), request=req)
    assert res.exit_code == 2 and res.consistent
    assert res.terminal["error"]["code"] == "unsupported-protocol-version"
    assert not (data_root / "jobs").exists()


@pytest.mark.parametrize(
    ("line", "code"),
    [
        ("{not json", "malformed-request"),
        ("[]", "malformed-request"),
        ('{"protocol_version":1}', "invalid-request"),
    ],
)
def test_malformed_requests(wav48: Path, data_root: Path, line: str, code: str) -> None:
    res = run_worker("separate", request=line, env=_env())
    assert res.exit_code == 2 and res.consistent
    assert [m["type"] for m in res.messages] == ["hello", "failed"]
    assert res.terminal["error"]["code"] == code


def test_stdin_closed_without_request() -> None:
    res = run_worker("separate", env=_env())
    assert res.exit_code == 2 and res.terminal["error"]["code"] == "request-missing"


def test_request_timeout() -> None:
    wp = WorkerProcess("separate", ["--request-timeout", "1"], env=_env())
    code = wp.proc.wait(timeout=30)
    msgs = []
    while (m := wp.messages.get(timeout=5)) is not None:
        msgs.append(m)
    wp.close_stdin()
    assert code == 2 and msgs[-1]["error"]["code"] == "request-timeout"


def _cancel(long_wav: Path, data_root: Path, mode: str):
    job_id = new_job_id()
    res = separate(
        long_wav,
        data_root,
        env=_env(SNDS_WORKER_TEST_SEGMENT_DELAY="0.25"),
        job_id=job_id,
        cancel_at_stage="separating",
        cancel_delay=1.0,
        cancel_mode=mode,
        timeout=60,
    )
    return job_id, res


@pytest.mark.parametrize(("mode", "reason"), [("message", "cancel-message"), ("eof", "stdin-eof")])
def test_cancellation(long_wav: Path, data_root: Path, mode: str, reason: str) -> None:
    job_id, res = _cancel(long_wav, data_root, mode)
    assert res.exit_code == 4, res.stderr
    assert res.consistent
    types = [m["type"] for m in res.messages]
    assert types.index("cancel_ack") < types.index("cancelled")
    ack = res.of_type("cancel_ack")[0]
    assert ack["reason"] == reason and ack["stage"] == "separating"
    final = res.terminal
    assert (
        final["type"] == "cancelled"
        and final["reason"] == reason
        and final["stage"] == "separating"
    )
    assert final["incomplete_audio_removed"] is True
    # Stopped within about one segment of the acknowledgement.
    t_ack = res.messages[types.index("cancel_ack")]["seq"]
    later_segments = [
        m for m in res.of_type("progress") if m["seq"] > t_ack and m["stage"] == "separating"
    ]
    assert len(later_segments) <= 1
    assert res.of_type("progress")[-1]["overall"] < 0.5
    job_dir = data_root / "jobs" / job_id
    assert sorted(p.name for p in job_dir.iterdir()) == ["job.log", "status.json"]
    status = json.loads((job_dir / "status.json").read_text())
    assert (
        status["state"] == "cancelled" and status["exit_code"] == 4 and status["reason"] == reason
    )
    assert "cancelled during separating" in (job_dir / "job.log").read_text()
    assert _no_entry(data_root, long_wav)


def test_cuda_failure_exits_3_and_cpu_retry_succeeds(wav48: Path, data_root: Path) -> None:
    job_id = new_job_id()
    res = separate(
        wav48, data_root, env=_env(SNDS_WORKER_TEST_FAULT="device-failure@2"), job_id=job_id
    )
    assert res.exit_code == 3 and res.consistent
    err = res.terminal["error"]
    assert err["code"] == "cuda-failed" and err["retry_on_cpu"] is True
    assert res.terminal["stage"] == "separating"
    assert _no_entry(data_root, wav48)
    status = json.loads((data_root / "jobs" / job_id / "status.json").read_text())
    assert status["state"] == "failed" and status["error"]["code"] == "cuda-failed"
    assert not (data_root / "jobs" / job_id / "package").exists()
    # The app's explicit CPU retry: a new process and a new job id.
    retry = separate(wav48, data_root, env=_env(), device="cpu")
    assert retry.exit_code == 0 and retry.of_type("device")[0]["requested"] == "cpu"


@pytest.mark.parametrize(
    ("fault", "failure"),
    [
        ("nan-output", "non-finite-samples"),
        ("drop-frame", "wrong-frame-count"),
        ("delete-stem", "missing-stem"),
    ],
)
def test_output_validation_failures(wav48: Path, data_root: Path, fault: str, failure: str) -> None:
    job_id = new_job_id()
    res = separate(wav48, data_root, env=_env(SNDS_WORKER_TEST_FAULT=fault), job_id=job_id)
    assert res.exit_code == 1 and res.consistent
    err = res.terminal["error"]
    assert err["code"] == "output-validation-failed"
    assert failure in [f["code"] for f in err["detail"]["failures"]]
    assert res.terminal["stage"] == "validating-output"
    assert _no_entry(data_root, wav48)
    assert not (data_root / "jobs" / job_id / "package").exists()


def test_internal_error(wav48: Path, data_root: Path) -> None:
    res = separate(wav48, data_root, env=_env(SNDS_WORKER_TEST_FAULT="internal-error"))
    assert res.exit_code == 1 and res.terminal["error"]["code"] == "internal-error"


def test_crash_before_publish_leaves_no_entry(wav48: Path, data_root: Path) -> None:
    job_id = new_job_id()
    res = separate(
        wav48, data_root, env=_env(SNDS_WORKER_TEST_FAULT="crash-before-publish"), job_id=job_id
    )
    assert res.exit_code == CRASH_EXIT_CODE
    assert res.terminal is None and not res.consistent  # abnormal exit: no terminal message
    assert _no_entry(data_root, wav48)
    jobs = list_jobs(data_root / "jobs", active_job_id=None)
    assert jobs == [
        {"job_id": job_id, "state": "publishing", "has_package": True, "abandoned": True}
    ]
    # The unpublished package is never a valid entry, even though its manifest exists.
    assert not inspect_entry(
        data_root / "jobs" / job_id / "package", pipeline_id=TEST_PIPELINE_ID
    ).valid
    # A later job for the same song succeeds regardless of the abandoned directory.
    assert separate(wav48, data_root, env=_env()).exit_code == 0


def test_crash_between_aside_and_publish_leaves_no_entry(wav48: Path, data_root: Path) -> None:
    first = separate(wav48, data_root, env=_env())
    entry = Path(first.terminal["cache_dir"])
    manifest = load_manifest(entry)
    manifest["schema_version"] = 99
    (entry / "manifest.json").write_text(json.dumps(manifest))
    job_id = new_job_id()
    res = separate(
        wav48, data_root, env=_env(SNDS_WORKER_TEST_FAULT="crash-after-aside"), job_id=job_id
    )
    assert res.exit_code == CRASH_EXIT_CODE and res.terminal is None
    assert not entry.exists()  # neither the old nor the new entry is visible
    assert (data_root / "jobs" / job_id / "replaced-0").is_dir()


def test_existing_valid_entry_is_reused(wav48: Path, data_root: Path) -> None:
    first = separate(wav48, data_root, env=_env())
    entry = Path(first.terminal["cache_dir"])
    before = (entry / "manifest.json").read_bytes()
    second = separate(wav48, data_root, env=_env())
    assert second.exit_code == 0 and second.terminal["publish"] == "reused-existing"
    assert (entry / "manifest.json").read_bytes() == before


def test_incompatible_entry_is_replaced_and_overrides_survive(wav48: Path, data_root: Path) -> None:
    first = separate(wav48, data_root, env=_env())
    entry = Path(first.terminal["cache_dir"])
    manifest = load_manifest(entry)
    manifest["pipeline"]["pipeline_id"] = "snds-p0-stale"
    (entry / "manifest.json").write_text(json.dumps(manifest))
    (entry / OVERRIDES_FILE).write_text('{"schema_version":1,"bpm":{"value":60.0}}')
    res = separate(wav48, data_root, env=_env())
    assert res.exit_code == 0 and res.terminal["publish"] == "replaced-existing"
    assert res.terminal["replaced_state"] == "incompatible"
    assert "replaced-cache-entry" in [m.get("code") for m in res.of_type("log")]
    assert inspect_entry(entry, pipeline_id=TEST_PIPELINE_ID, deep=True).valid
    assert json.loads((entry / OVERRIDES_FILE).read_text())["bpm"]["value"] == 60.0


def test_fingerprint_mismatch(wav48: Path, data_root: Path) -> None:
    req = make_request(wav48, new_job_id(), data_root=data_root)
    req["source"]["sha256"] = "0" * 64
    res = separate(wav48, data_root, env=_env(), request=req)
    assert res.exit_code == 1 and res.terminal["error"]["code"] == "source-fingerprint-mismatch"
    assert res.terminal["stage"] == "validating-input"
    req = make_request(wav48, new_job_id(), data_root=data_root)
    req["source"]["size_bytes"] += 1
    res = separate(wav48, data_root, env=_env(), request=req)
    assert res.terminal["error"]["code"] == "source-fingerprint-mismatch"
    assert res.terminal["error"]["detail"]["actual_size"] == wav48.stat().st_size


def test_missing_and_unsupported_sources(tmp_path: Path, wav48: Path, data_root: Path) -> None:
    req = make_request(wav48, new_job_id(), data_root=data_root)
    req["source"]["path"] = str(tmp_path / "gone.wav")
    res = separate(wav48, data_root, env=_env(), request=req)
    assert res.exit_code == 1 and res.terminal["error"]["code"] == "source-not-found"

    import numpy as np

    three = write_wav(tmp_path / "3ch.wav", np.zeros((44100, 3), np.float32), 44100)
    res = separate(three, data_root, env=_env())
    assert res.terminal["error"]["code"] == "unsupported-channel-count"


def test_job_id_collision(wav48: Path, data_root: Path) -> None:
    job_id = new_job_id()
    (data_root / "jobs" / job_id).mkdir(parents=True)
    res = separate(wav48, data_root, env=_env(), job_id=job_id)
    assert res.exit_code == 1 and res.terminal["error"]["code"] == "job-exists"


def test_unknown_control_lines_are_ignored(long_wav: Path, data_root: Path) -> None:
    wp = WorkerProcess("separate", env=_env(SNDS_WORKER_TEST_SEGMENT_DELAY="0.05"))
    wp.send(make_request(long_wav, new_job_id(), data_root=data_root))
    wp.send({"type": "pause"})
    msgs = []
    while (m := wp.messages.get(timeout=60)) is not None:
        msgs.append(m)
    wp.close_stdin()
    assert wp.proc.wait(timeout=30) == 0
    assert "control-message-ignored" in [m.get("code") for m in msgs if m["type"] == "log"]


def test_killed_worker_is_an_abnormal_exit(long_wav: Path, data_root: Path) -> None:
    """What the app sees when it must TerminateJobObject a hung worker (or the worker dies)."""
    job_id = new_job_id()
    wp = WorkerProcess("separate", env=_env(SNDS_WORKER_TEST_SEGMENT_DELAY="0.25"))
    wp.send(make_request(long_wav, job_id, data_root=data_root))
    msgs = []
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        m = wp.messages.get(timeout=60)
        msgs.append(m)
        if (
            m["type"] == "progress"
            and m["stage"] == "separating"
            and m.get("detail", {}).get("segments_done", 0) >= 2
        ):
            break
    wp.kill()
    code = wp.proc.wait(timeout=30)
    wp.close_stdin()
    assert code != 0  # Popen.kill = TerminateProcess(1); only the missing terminal message counts
    assert not any(m and m["type"] in ("result", "failed", "cancelled") for m in msgs)
    assert _no_entry(data_root, long_wav)
    jobs = list_jobs(data_root / "jobs")
    assert jobs[0]["state"] == "running" and jobs[0]["abandoned"]
    # App-side cleanup of the abandoned job keeps diagnostics but no audio.
    shutil.rmtree(data_root / "jobs" / job_id / "package")
    assert (data_root / "jobs" / job_id / "job.log").exists()
