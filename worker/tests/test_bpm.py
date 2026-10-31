from __future__ import annotations

import hashlib
import shutil
from pathlib import Path

import numpy as np
import pytest

from helpers import run_job, write_wav
from snds_worker import bpm, model, schema
from snds_worker.cancel import CancelToken
from snds_worker.decode import DecodedSource
from snds_worker.errors import JobCancelled


def quantized_beats(bpm_value: float, seconds: float = 30.0, frame: float = 0.02) -> np.ndarray:
    """Beat times as beat_this reports them: on a 20 ms frame grid."""
    return np.round(np.arange(0, seconds, 60.0 / bpm_value) / frame) * frame


def source(seconds: float = 2.0) -> DecodedSource:
    audio = np.zeros((int(44_100 * seconds), 2), dtype=np.float32)
    return DecodedSource(audio, 44_100, 2, len(audio), "wav", "FLOAT")


@pytest.mark.parametrize("value", [60.0, 87.5, 120.0, 128.0, 174.0])
def test_tempo_is_recovered_below_the_frame_quantization(value: float) -> None:
    bpm_value, confidence = bpm.tempo_from_beats(quantized_beats(value))

    assert bpm_value == pytest.approx(value, abs=0.6)  # a 20 ms grid alone would be off by ~2
    assert confidence > 0.5


def test_regular_beats_have_full_confidence() -> None:
    value, confidence = bpm.tempo_from_beats(np.arange(0, 30, 0.5))

    assert value == pytest.approx(120.0)
    assert confidence == 1.0


def test_outliers_lower_confidence_but_not_the_tempo() -> None:
    beats = np.arange(0, 30, 0.5)
    beats[10:] += 0.08  # one irregular interval of 0.58 s
    value, confidence = bpm.tempo_from_beats(beats)

    assert value == pytest.approx(120.0, rel=0.01)
    assert confidence == pytest.approx(1 - 1 / 59, abs=0.01)


def test_too_few_beats_have_no_tempo() -> None:
    assert bpm.tempo_from_beats(np.array([])) is None
    assert bpm.tempo_from_beats(np.array([1.0, 1.5])) is None
    assert bpm.tempo_from_beats(np.array([1.0, 1.0, 1.0])) is None  # zero intervals


def test_success_outcome_has_alternatives() -> None:
    outcome = bpm.outcome_from_beats(quantized_beats(120.0))

    assert outcome.value == pytest.approx(120.0, abs=0.6)
    assert outcome.alternatives == [
        pytest.approx(outcome.value * 0.5),
        pytest.approx(outcome.value * 2),
    ]
    assert outcome.error is None
    assert outcome.beats == len(quantized_beats(120.0))


@pytest.mark.parametrize(
    ("beats", "reason"),
    [
        (np.array([0.5, 1.0]), "too few beats detected (2)"),
        (np.arange(0, 1.0, 0.02), "implausible tempo"),  # 3000 BPM
        (np.arange(0, 400, 10.0), "implausible tempo"),  # 6 BPM
    ],
)
def test_failed_outcome_is_nullable_and_explained(beats: np.ndarray, reason: str) -> None:
    outcome = bpm.outcome_from_beats(beats)

    assert outcome.value is None
    assert outcome.confidence == 0.0
    assert outcome.alternatives == []
    assert outcome.beats == len(beats)
    assert reason in outcome.error


def test_json_matches_the_song_schema_for_success_and_failure() -> None:
    for outcome in (bpm.outcome_from_beats(quantized_beats(100.0)), bpm.failed("no model")):
        data = outcome.to_json()
        assert data["analyzer"] == {
            "name": "beat_this",
            "version": "1.1.0",
            "checkpointSha256": model.BEAT_MODEL_SHA256,
        }
        assert data["userOverride"] is None
        assert set(data) == {
            "value",
            "confidence",
            "beats",
            "alternatives",
            "error",
            "analyzer",
            "userOverride",
        }
        assert schema.validate_song_metadata(_song_with(data)) == []


def _song_with(bpm_json: dict) -> dict:
    frames, rate = 100, 44_100
    return schema.build_song_metadata(
        source={
            "fileName": "a.wav",
            "path": "C:\\a.wav",
            "sizeBytes": 1,
            "sha256": "ab" * 32,
            "container": "wav",
            "sampleRate": rate,
            "channels": 2,
            "frames": frames,
            "durationSeconds": frames / rate,
        },
        stems={n: schema.stem_entry(frames, rate, 0.5, n) for n in schema.STEM_NAMES},
        bpm=bpm_json,
        worker={"version": "0.0.1", "protocol": 1, "torch": "2.1.2+cpu", "device": "cpu"},
        prepared_at="2026-10-04T12:00:00Z",
    )


# --- The pipeline stage ----------------------------------------------------------------------


@pytest.fixture
def beat_checkpoint(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    content = b"fake beat checkpoint" * 50
    monkeypatch.setattr(model, "BEAT_MODEL_SIZE", len(content))
    monkeypatch.setattr(model, "BEAT_MODEL_SHA256", hashlib.sha256(content).hexdigest())
    directory = tmp_path / "models" / "beat_this"
    directory.mkdir(parents=True)
    (directory / model.BEAT_MODEL_FILENAME).write_bytes(content)
    return directory


def analyze(directory: Path, device: str = "cpu", token: CancelToken | None = None):
    seen: list[float] = []
    outcome = bpm.make_analyzer(directory)(source(), device, token or CancelToken(), seen.append)
    return outcome, seen


@pytest.mark.parametrize("state", ["missing", "truncated", "wrong content"])
def test_missing_or_damaged_checkpoint_records_an_error(tmp_path: Path, state: str) -> None:
    directory = tmp_path / "beat_this"
    directory.mkdir()
    if state != "missing":
        content = b"x" * (10 if state == "truncated" else model.BEAT_MODEL_SIZE)
        (directory / model.BEAT_MODEL_FILENAME).write_bytes(content)

    outcome, _ = analyze(directory)

    assert outcome.value is None
    assert "missing or damaged" in outcome.error
    assert "bootstrap.ps1" in outcome.error


def test_stage_returns_the_outcome_from_detected_beats(beat_checkpoint, monkeypatch) -> None:
    calls = []

    def fake_detect(audio, rate, checkpoint, device, token, progress):
        calls.append((rate, checkpoint.name, device))
        progress(1.0)
        return quantized_beats(96.0)

    monkeypatch.setattr(bpm, "detect_beats", fake_detect)

    outcome, seen = analyze(beat_checkpoint)

    assert outcome.value == pytest.approx(96.0, abs=0.6)
    assert calls == [(44_100, "small0.ckpt", "cpu")]
    assert seen == [1.0]


def test_analysis_errors_never_raise(beat_checkpoint, monkeypatch) -> None:
    def explode(*_args):
        raise RuntimeError("model blew up")

    monkeypatch.setattr(bpm, "detect_beats", explode)

    outcome, _ = analyze(beat_checkpoint)

    assert outcome.value is None
    assert "model blew up" in outcome.error
    assert outcome.to_json()["analyzer"]["name"] == "beat_this"


def test_cuda_errors_are_retried_on_the_cpu(beat_checkpoint, monkeypatch) -> None:
    devices = []

    def flaky(audio, rate, checkpoint, device, token, progress):
        devices.append(device)
        if device == "cuda":
            raise RuntimeError("CUDA error: unknown error")
        return quantized_beats(110.0)

    monkeypatch.setattr(bpm, "detect_beats", flaky)

    outcome, _ = analyze(beat_checkpoint, device="cuda")

    assert devices == ["cuda", "cpu"]
    assert outcome.value == pytest.approx(110.0, abs=0.6)


def test_cancellation_propagates(beat_checkpoint, monkeypatch) -> None:
    def cancelled(*_args):
        raise JobCancelled("requested")

    monkeypatch.setattr(bpm, "detect_beats", cancelled)

    with pytest.raises(JobCancelled):
        analyze(beat_checkpoint)


def test_pipeline_keeps_the_job_when_bpm_fails(tmp_path: Path) -> None:
    wav = write_wav(tmp_path / "song.wav", seconds=3)

    run = run_job(tmp_path, wav, analyze_bpm=lambda *_a: bpm.failed("no beats here", 1))

    assert run.exit_code == 0, run.terminal
    assert run.job.bpm.value is None
    assert run.job.bpm.error == "no beats here"
    assert run.stages == list(
        ("decoding", "separating", "normalizing", "analyzing_bpm", "validating", "finalizing")
    )


def test_pipeline_hands_the_source_mix_to_the_analyzer(tmp_path: Path) -> None:
    wav = write_wav(tmp_path / "song.wav", 48_000, 3.0, 1)
    seen = {}

    def analyzer(decoded, device, token, progress):
        seen.update(rate=decoded.sample_rate, channels=decoded.channels, frames=decoded.frames)
        progress(0.5)
        return bpm.outcome_from_beats(quantized_beats(100.0))

    run = run_job(tmp_path, wav, analyze_bpm=analyzer)

    assert run.exit_code == 0
    assert seen == {"rate": 48_000, "channels": 1, "frames": 144_000}
    assert run.job.bpm.value == pytest.approx(100.0, abs=0.6)
    assert any(e["type"] == "progress" and e["stage"] == "analyzing_bpm" for e in run.events)


# --- Real checkpoint (opt in) ----------------------------------------------------------------


def drum_loop(rate: int, bpm_value: float, seconds: float) -> np.ndarray:
    """Kick, snare and hats at a fixed tempo (the spike's four-on-the-floor feel)."""
    rng = np.random.default_rng(5)
    audio = np.zeros(int(rate * seconds))
    beat = 60.0 / bpm_value
    t_hit = np.arange(int(0.25 * rate)) / rate
    kick = np.sin(2 * np.pi * (50 + 110 * np.exp(-t_hit / 0.04)) * t_hit) * np.exp(-t_hit / 0.1)
    snare = rng.standard_normal(len(t_hit)) * np.exp(-t_hit / 0.05) * 0.5
    hat = rng.standard_normal(len(t_hit) // 4) * np.exp(-t_hit[: len(t_hit) // 4] / 0.01) * 0.2
    for index in range(int(seconds / beat)):
        start = int(index * beat * rate)
        for offset, sound in ((0, kick if index % 2 == 0 else snare), (beat / 2, hat)):
            begin = start + int(offset * rate)
            end = min(len(audio), begin + len(sound))
            audio[begin:end] += sound[: end - begin]
    return (audio / np.abs(audio).max() * 0.8).astype(np.float32)


@pytest.mark.parametrize("target", [100.0, 128.0])
def test_real_beat_this_finds_the_tempo(tmp_path: Path, target: float, real_checkpoint) -> None:
    directory = tmp_path / "beat_this"
    directory.mkdir()
    shutil.copy(real_checkpoint("beat_this"), directory / model.BEAT_MODEL_FILENAME)
    mono = drum_loop(44_100, target, 30.0)
    decoded = DecodedSource(np.stack([mono, mono], axis=1), 44_100, 2, len(mono), "wav", "FLOAT")
    seen: list[float] = []

    outcome = bpm.make_analyzer(directory)(decoded, "cpu", CancelToken(), seen.append)

    assert outcome.error is None, outcome
    assert outcome.value == pytest.approx(target, abs=1.5)
    assert outcome.confidence > 0.5
    assert seen[-1] == 1.0
