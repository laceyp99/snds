"""Integration tests with the real pinned htdemucs checkpoint (skipped when it is absent)."""

from __future__ import annotations

from pathlib import Path

import pytest

from audio_fixtures import synth_song, write_mp3, write_wav
from conftest import load_manifest, separate
from snds_worker.cache import PIPELINE_ID, inspect_entry


@pytest.fixture
def song(tmp_path: Path) -> Path:
    frames = 48000 * 6 + 123
    return write_wav(
        tmp_path / "song.wav",
        synth_song(frames / 48000 + 0.01, sr=48000, seed=21)[:frames],
        48000,
        "PCM_24",
    )


def _assert_valid(res, device_prefix: str) -> dict:
    assert res.exit_code == 0, res.stderr[-3000:]
    assert res.consistent
    assert res.of_type("device")[0]["selected"].startswith(device_prefix)
    entry = Path(res.terminal["cache_dir"])
    r = inspect_entry(entry, pipeline_id=PIPELINE_ID, deep=True)
    assert r.valid, r.reasons
    m = load_manifest(entry)
    metrics = m["validation"]["metrics"]
    assert metrics["alignment"]["lag_samples"] == 0
    assert metrics["residual_db_inband"] < -20  # spike: about -33 dB
    assert m["pipeline"]["pipeline_id"] == PIPELINE_ID
    assert m["pipeline"]["separation"]["shifts"] == 0
    assert m["compute"]["selected"] == res.of_type("device")[0]["selected"]
    return m


@pytest.mark.model
@pytest.mark.cuda
def test_real_separation_on_cuda(song: Path, model_root: Path) -> None:
    m = _assert_valid(separate(song, model_root), "cuda")
    assert m["producer"]["packages"]["torch"].endswith("+cu128")
    assert m["source"]["samplerate"] == 48000 and m["stems"]["frames"] == 48000 * 6 + 123


@pytest.mark.model
def test_real_separation_forced_cpu(song: Path, model_root: Path) -> None:
    res = separate(song, model_root, device="cpu")
    m = _assert_valid(res, "cpu")
    assert m["compute"]["reason"] == "forced-cpu"


@pytest.mark.model
def test_real_separation_mp3_cpu_without_gpu(tmp_path: Path, model_root: Path) -> None:
    frames = 44100 * 5 + 7
    mp3 = write_mp3(tmp_path / "s.mp3", synth_song(frames / 44100 + 0.01, seed=4)[:frames], 44100)
    res = separate(mp3, model_root, env={"CUDA_VISIBLE_DEVICES": "-1"})
    m = _assert_valid(res, "cpu")
    assert m["compute"]["reason"] == "cuda-not-available"
    assert m["source"]["format"] == "MP3" and m["stems"]["frames"] == frames


@pytest.mark.model
@pytest.mark.cuda
def test_real_cuda_midjob_failure_then_cpu_retry(song: Path, model_root: Path) -> None:
    res = separate(song, model_root, env={"SNDS_WORKER_TEST_FAULT": "device-failure@1"})
    assert res.exit_code == 3 and res.consistent, res.stderr[-3000:]
    err = res.terminal["error"]
    assert err["code"] == "cuda-failed" and err["retry_on_cpu"] is True
    assert "injected" in err["message"]
    retry = separate(
        song, model_root, device="cpu", env={"SNDS_WORKER_TEST_FAULT": "device-failure@1"}
    )
    _assert_valid(retry, "cpu")  # the fault only triggers on CUDA


@pytest.mark.model
@pytest.mark.cuda
def test_real_cuda_probe_failure_falls_back_to_cpu(song: Path, model_root: Path) -> None:
    res = separate(song, model_root, env={"SNDS_WORKER_TEST_FAULT": "cuda-probe"})
    m = _assert_valid(res, "cpu")
    assert m["compute"]["reason"] == "cuda-probe-failed"
    assert "cuda-fallback" in [w["code"] for w in m["warnings"]]


def test_explicit_cuda_without_a_device_exits_3(song: Path, data_root: Path) -> None:
    """Device selection happens before the model is loaded, so no checkpoint is needed."""
    res = separate(song, data_root, device="cuda", env={"CUDA_VISIBLE_DEVICES": "-1"})
    assert res.exit_code == 3 and res.consistent
    assert res.terminal["error"]["code"] == "cuda-unavailable"
    assert res.terminal["stage"] == "loading-model"


@pytest.mark.model
def test_real_cancel_mid_separation_on_cpu(tmp_path: Path, model_root: Path) -> None:
    src = write_wav(tmp_path / "long.wav", synth_song(40.0, seed=8), 44100)
    res = separate(src, model_root, device="cpu", cancel_at_stage="separating", cancel_delay=0.5)
    assert res.exit_code == 4 and res.consistent, res.stderr[-3000:]
    assert res.terminal["stage"] == "separating"
    acks = res.of_type("cancel_ack")
    assert len(acks) == 1
    assert not (model_root / "cache").exists() or not any((model_root / "cache").iterdir())
