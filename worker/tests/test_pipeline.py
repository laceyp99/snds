from __future__ import annotations

import errno
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from helpers import (
    STAGE_ORDER,
    RecordingWriter,
    make_request,
    run_job,
    synth_audio,
    write_mp3,
    write_wav,
)
from snds_worker import jobs_protocol, normalize
from snds_worker.cancel import CancelToken
from snds_worker.errors import JobFailure
from snds_worker.fake_separator import FakeSeparator

STEMS = ("drums", "bass", "other", "vocals")


def check_event_contract(run) -> None:
    """The ordering rules of docs/protocol.md section 3.3 that hold for every job."""
    events = run.events
    assert events[0]["type"] == "accepted"
    assert events[-1]["type"] in jobs_protocol.TERMINAL_TYPES
    assert [e["type"] for e in events if e["type"] in jobs_protocol.TERMINAL_TYPES] == [
        events[-1]["type"]
    ]
    assert [e["seq"] for e in events] == list(range(1, len(events) + 1))
    assert run.exit_code == jobs_protocol.exit_code_for(events[-1])

    stages = run.stages
    assert stages == STAGE_ORDER[: len(stages)]
    current = None
    last_fraction: dict[str, float] = {}
    for event in events:
        if event["type"] == "stage":
            current = event["stage"]
            assert event["index"] == STAGE_ORDER.index(current) + 1
            assert event["count"] == 6
        elif event["type"] == "progress":
            assert event["stage"] == current
            assert 0.0 <= event["fraction"] <= 1.0
            assert event["fraction"] >= last_fraction.get(current, 0.0)
            last_fraction[current] = event["fraction"]


@pytest.mark.parametrize(
    ("rate", "channels", "subtype"),
    [
        (44_100, 2, "PCM_16"),
        (48_000, 2, "PCM_24"),
        (22_050, 1, "PCM_16"),
        (96_000, 2, "FLOAT"),
        (32_000, 1, "DOUBLE"),
    ],
)
def test_wav_job_writes_aligned_stems(tmp_path: Path, rate, channels, subtype) -> None:
    source = write_wav(tmp_path / "song.wav", rate, 7.3, channels, subtype)

    run = run_job(tmp_path, source)

    assert run.exit_code == 0, run.events[-1]
    check_event_contract(run)
    assert run.stages == STAGE_ORDER
    assert run.types[0] == "accepted"
    assert run.events[0] == {
        "protocol": 1,
        "type": "accepted",
        "jobId": "job-1",
        "device": "cpu",
        "seq": 1,
    }
    assert run.terminal["type"] == "result"

    frames = sf.info(str(source)).frames
    assert run.job.source.frames == frames
    assert sorted(p.name for p in run.package.iterdir()) == sorted(f"{n}.wav" for n in STEMS)
    total = np.zeros((frames, 2))
    for name in STEMS:
        info = sf.info(str(run.package / f"{name}.wav"))
        assert (info.frames, info.samplerate, info.channels, info.subtype) == (
            frames,
            rate,
            2,
            "FLOAT",
        )
        audio, _ = sf.read(str(run.package / f"{name}.wav"), dtype="float32")
        total += audio
        assert run.job.stems[name].peak == pytest.approx(float(np.abs(audio).max()))
    reference, _ = sf.read(str(source), dtype="float32", always_2d=True)
    reference = np.repeat(reference, 2, axis=1) if channels == 1 else reference
    residual = np.sum((total - reference) ** 2) / np.sum(reference**2)
    assert residual < 1e-3  # only the resampling round trip differs from the source


def test_mp3_job(tmp_path: Path) -> None:
    source = write_mp3(tmp_path / "song.mp3", 44_100, 8.0)

    run = run_job(tmp_path, source)

    assert run.exit_code == 0, run.events[-1]
    assert run.job.source.container == "mp3"
    assert sf.info(str(run.package / "drums.wav")).frames == run.job.source.frames


def test_peaks_above_one_are_preserved(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "hot.wav", 44_100, 6.0, 2, "FLOAT", peak=2.5)

    run = run_job(tmp_path, source)

    assert run.exit_code == 0, run.events[-1]
    assert run.job.stems["drums"].peak == pytest.approx(0.4 * 2.5, rel=1e-4)
    total_peak = sum(stem.peak for stem in run.job.stems.values())
    assert total_peak > 1.0  # nothing was clipped or rescaled


def test_silent_source_is_accepted(tmp_path: Path) -> None:
    source = tmp_path / "silence.wav"
    sf.write(str(source), np.zeros((44_100 * 4, 2), dtype=np.float32), 44_100)

    run = run_job(tmp_path, source)

    assert run.exit_code == 0, run.events[-1]
    assert all(stem.peak == 0.0 for stem in run.job.stems.values())
    assert any("too quiet" in w for w in run.job.validation.warnings)


def test_alignment_windows_avoid_fades(tmp_path: Path) -> None:
    audio = synth_audio(44_100, 14.0)
    audio[-4 * 44_100 :] *= 1e-4  # a fade-out into the noise floor
    audio[: 2 * 44_100] *= 1e-4  # and a fade-in
    source = tmp_path / "faded.wav"
    sf.write(str(source), audio, 44_100, subtype="FLOAT")

    run = run_job(tmp_path, source)

    assert run.exit_code == 0, run.events[-1]
    alignment = run.job.validation.alignment
    assert alignment["startSeconds"] >= 1.9
    assert alignment["endSeconds"] + 3.0 <= 14.0 - 1.0
    assert alignment["startLag"] == alignment["endLag"] == 0
    assert run.job.validation.warnings == []


def test_very_short_source_is_accepted(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "blip.wav", 44_100, 0.4)

    run = run_job(tmp_path, source)

    assert run.exit_code == 0, run.events[-1]


def test_device_choice_is_reported(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)

    run = run_job(tmp_path, source, preference="auto", device="cuda")

    assert run.events[0]["device"] == "cuda"


def test_progress_is_reported_for_separation(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)

    run = run_job(tmp_path, source, separator=FakeSeparator(segments=100, segment_seconds=0.002))

    fractions = [
        e["fraction"] for e in run.events if e["type"] == "progress" and e["stage"] == "separating"
    ]
    assert fractions[-1] == 1.0
    assert len(fractions) > 3
    assert "etaSeconds" not in run.events[1]


# --- Failures ------------------------------------------------------------------------------


def failure_of(run, code: str) -> dict:
    check_event_contract(run)
    assert run.terminal["type"] == "failure", run.terminal
    assert run.terminal["code"] == code, run.terminal
    assert run.exit_code == 13
    assert not run.package.exists()  # no incomplete audio is left behind
    return run.terminal


def test_missing_source(tmp_path: Path) -> None:
    run = run_job(tmp_path, tmp_path / "missing.wav")

    failure = failure_of(run, "source_unreadable")
    assert failure["suggestedAction"] == "relocate_source"
    assert run.stages == ["decoding"]


def test_source_size_changed(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)
    request = make_request(tmp_path, source)
    source.write_bytes(source.read_bytes() + b"\0" * 10)

    failure_of(run_job(tmp_path, source, request=request), "source_changed")


def test_source_hash_changed(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)
    request = make_request(tmp_path, source)
    data = bytearray(source.read_bytes())
    data[-1] ^= 0xFF  # same size, different content
    source.write_bytes(bytes(data))

    failure_of(run_job(tmp_path, source, request=request), "source_changed")


def test_unsupported_sources(tmp_path: Path) -> None:
    flac = tmp_path / "song.flac"
    sf.write(str(flac), np.zeros((4410, 2), dtype=np.float32), 44_100, format="FLAC")
    failure_of(run_job(tmp_path, flac), "source_unsupported")

    text = tmp_path / "notes.wav"
    text.write_text("this is not audio", encoding="utf-8")
    failure_of(run_job(tmp_path, text), "source_unsupported")

    surround = tmp_path / "surround.wav"
    sf.write(str(surround), np.zeros((4410, 6), dtype=np.float32), 44_100)
    failure = failure_of(run_job(tmp_path, surround), "source_unsupported")
    assert "6 channels" in failure["message"]

    adpcm = tmp_path / "adpcm.wav"
    sf.write(str(adpcm), np.zeros((4410, 1), dtype=np.float32), 44_100, subtype="IMA_ADPCM")
    failure_of(run_job(tmp_path, adpcm), "source_unsupported")


def test_wav_with_a_damaged_header_is_rejected(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=4)
    source.write_bytes(source.read_bytes()[:30])

    failure_of(run_job(tmp_path, source), "source_unsupported")


def test_wav_with_non_finite_samples_is_rejected(tmp_path: Path) -> None:
    audio = np.zeros((44_100, 2), dtype=np.float32)
    audio[100, 0] = np.nan
    source = tmp_path / "nan.wav"
    sf.write(str(source), audio, 44_100, subtype="FLOAT")

    failure_of(run_job(tmp_path, source), "decode_failed")


def test_model_missing_failure_carries_the_fetch_action(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)

    def factory(_device, _dir):
        raise JobFailure("model_missing", "gone", suggested_action="fetch_model")

    failure = failure_of(run_job(tmp_path, source, factory=factory), "model_missing")
    assert failure["suggestedAction"] == "fetch_model"


@pytest.mark.parametrize(
    ("kind", "device", "code", "retryable", "action"),
    [
        ("oom", "cuda", "out_of_memory", True, "retry_cpu"),
        ("oom", "cpu", "out_of_memory", False, None),
        ("cuda", "cuda", "cuda_failed", True, "retry_cpu"),
        ("error", "cuda", "separation_failed", False, None),
    ],
)
def test_separator_runtime_failures(tmp_path: Path, kind, device, code, retryable, action) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)

    run = run_job(
        tmp_path, source, separator=FakeSeparator(fail=kind), preference="auto", device=device
    )

    failure = failure_of(run, code)
    assert failure["retryable"] is retryable
    assert failure.get("suggestedAction") == action


def test_disk_full_while_writing_stems(tmp_path: Path, monkeypatch) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)

    def full(*_args, **_kwargs):
        raise OSError(errno.ENOSPC, "No space left on device")

    monkeypatch.setattr(normalize.sf, "write", full)

    failure = failure_of(run_job(tmp_path, source), "disk_full")
    assert failure["suggestedAction"] == "free_disk_space"


class ShiftedSeparator(FakeSeparator):
    """Delays every stem by `shift` samples: a misaligned separator."""

    def __init__(self, shift: int) -> None:
        super().__init__()
        self.shift = shift

    def separate(self, wav, progress, token):
        import torch

        out = super().separate(wav, progress, token)
        return torch.roll(out, self.shift, dims=-1)


@pytest.mark.parametrize("shift", [1, 40, 700])
def test_misaligned_stems_fail_validation(tmp_path: Path, shift: int) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=8)

    run = run_job(tmp_path, source, separator=ShiftedSeparator(shift))

    failure = failure_of(run, "validation_failed")
    assert run.stages[-1] == "validating"
    assert "align" in failure["message"] or "match" in failure["message"]


def test_nan_stem_output_fails_normalization(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)

    class NanSeparator(FakeSeparator):
        def separate(self, wav, progress, token):
            out = super().separate(wav, progress, token)
            out[2, 0, 5] = float("nan")
            return out

    failure_of(run_job(tmp_path, source, separator=NanSeparator()), "normalization_failed")


def test_wrong_separator_shape_fails(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)

    class ThreeStems(FakeSeparator):
        def separate(self, wav, progress, token):
            return super().separate(wav, progress, token)[:3]

    failure_of(run_job(tmp_path, source, separator=ThreeStems()), "separation_failed")


def test_unexpected_exception_is_an_internal_error(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=2)

    def explode(*_args):
        raise RuntimeError("boom")

    run = run_job(tmp_path, source, analyze_bpm=explode)

    check_event_contract(run)
    assert run.terminal["code"] == "internal_error"
    assert run.exit_code == 14
    assert "boom" in run.terminal["details"]
    assert not run.package.exists()


# --- Cancellation --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "stage", ["decoding", "separating", "normalizing", "analyzing_bpm", "validating"]
)
def test_cancel_at_each_stage(tmp_path: Path, stage: str) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)
    token = CancelToken()
    artwork = Path(make_request(tmp_path, source).job_dir) / "artwork.png"
    artwork.write_bytes(b"png")

    def on_event(event):
        if event["type"] == "stage" and event["stage"] == stage:
            token.trigger("requested")

    run = run_job(
        tmp_path,
        source,
        token=token,
        on_event=on_event,
        separator=FakeSeparator(segments=3),
    )

    check_event_contract(run)
    assert run.terminal == {**run.terminal, "type": "cancelled", "reason": "requested"}
    assert run.exit_code == 12
    assert run.stages[-1] == stage
    assert not run.package.exists()
    assert artwork.exists()  # the app's own files stay


def test_cancel_in_the_middle_of_separation(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)
    token = CancelToken()

    class Cancelling(FakeSeparator):
        def separate(self, wav, progress, token_):
            progress(0.5)
            token.trigger("stdin_closed")
            return super().separate(wav, progress, token_)

    run = run_job(tmp_path, source, token=token, separator=Cancelling())

    assert run.terminal["type"] == "cancelled"
    assert run.terminal["reason"] == "stdin_closed"
    assert run.stages == ["decoding", "separating"]
    assert not run.package.exists()


def test_cancel_after_some_stems_are_written_removes_them(tmp_path: Path, monkeypatch) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)
    token = CancelToken()
    real_write = normalize.write_stem
    written = []

    def write_then_cancel(name, *args, **kwargs):
        stem = real_write(name, *args, **kwargs)
        written.append(name)
        if len(written) == 2:
            token.trigger("requested")
        return stem

    monkeypatch.setattr(normalize, "write_stem", write_then_cancel)

    run = run_job(tmp_path, source, token=token)

    assert run.terminal["type"] == "cancelled"
    assert written == ["drums", "bass"]
    assert not run.package.exists()


def test_cancel_during_finalize_is_honoured_by_the_finalizer(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)
    token = CancelToken()

    def finalize(job, token_):
        token.trigger("requested")
        token_.raise_if_cancelled()
        return {}

    run = run_job(tmp_path, source, token=token, finalize=finalize)

    assert run.terminal["type"] == "cancelled"
    assert run.stages == STAGE_ORDER
    assert not run.package.exists()


def test_broken_stdout_stops_the_job(tmp_path: Path) -> None:
    source = write_wav(tmp_path / "song.wav", seconds=3)
    writer = RecordingWriter(broken=True)  # the app closed its end of stdout

    run = run_job(tmp_path, source, writer=writer)

    assert writer.broken
    assert run.exit_code == 12  # nothing can be reported; the job stops as cancelled
    assert not run.package.exists()
