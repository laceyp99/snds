from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
import soundfile as sf

from helpers import sha256_of, write_mp3, write_wav
from snds_worker import decode
from snds_worker.cancel import CancelToken
from snds_worker.errors import JobCancelled, JobFailure


def noop(_fraction: float) -> None:
    pass


@pytest.mark.parametrize("extra_frames", [0, 1])
def test_duration_limit_is_inclusive(monkeypatch: pytest.MonkeyPatch, extra_frames: int) -> None:
    info = SimpleNamespace(
        format="RF64",
        subtype="PCM_16",
        channels=2,
        samplerate=8000,
        frames=8000 * 30 * 60 + extra_frames,
    )
    monkeypatch.setattr(sf, "info", lambda _: info)
    if extra_frames:
        with pytest.raises(JobFailure, match="30 minutes"):
            decode.inspect_source(Path("long.wav"))
    else:
        assert decode.inspect_source(Path("boundary.wav"))[0] == "wav"


@pytest.mark.parametrize("subtype", ["PCM_U8", "PCM_16", "PCM_24", "PCM_32", "FLOAT", "DOUBLE"])
def test_supported_wav_sample_formats(tmp_path: Path, subtype: str) -> None:
    path = write_wav(tmp_path / "a.wav", 48_000, 1.0, 2, subtype)

    decoded = decode.decode_source(path, CancelToken(), noop)

    assert decoded.container == "wav"
    assert (decoded.sample_rate, decoded.channels, decoded.frames) == (48_000, 2, 48_000)
    assert decoded.audio.dtype == np.float32
    assert decoded.audio.shape == (48_000, 2)


@pytest.mark.parametrize("rate", [8_000, 192_000])
def test_rate_limits_are_inclusive(tmp_path: Path, rate: int) -> None:
    path = write_wav(tmp_path / "a.wav", rate, 0.5, 1)

    assert decode.decode_source(path, CancelToken(), noop).sample_rate == rate


def test_rates_outside_the_supported_range_are_rejected(tmp_path: Path) -> None:
    for rate in (4_000, 384_000):
        path = tmp_path / f"{rate}.wav"
        sf.write(str(path), np.zeros((rate // 10, 1), dtype=np.float32), rate)

        with pytest.raises(JobFailure) as error:
            decode.decode_source(path, CancelToken(), noop)

        assert error.value.code == "source_unsupported"
        assert "Hz" in error.value.message


def test_mono_mp3(tmp_path: Path) -> None:
    path = write_mp3(tmp_path / "a.mp3", 44_100, 5.0, channels=1)

    decoded = decode.decode_source(path, CancelToken(), noop)

    assert (decoded.container, decoded.channels) == ("mp3", 1)
    assert abs(decoded.frames - 5 * 44_100) < 3 * 1152


def test_truncated_mp3_decode_is_rejected(tmp_path: Path) -> None:
    path = write_mp3(tmp_path / "a.mp3", 44_100, 5.0)

    decode.check_mp3_complete(path, decode.decode_source(path, CancelToken(), noop).frames)
    with pytest.raises(JobFailure) as error:
        decode.check_mp3_complete(path, 44_100)  # as if the decoder stopped after one second

    assert error.value.code == "decode_failed"


def test_verify_source_reports_progress_and_polls_cancellation(tmp_path: Path) -> None:
    path = write_wav(tmp_path / "a.wav", 44_100, 3.0)
    seen: list[float] = []

    decode.verify_source(path, path.stat().st_size, sha256_of(path), CancelToken(), seen.append)

    assert seen[-1] == 1.0

    token = CancelToken()
    token.trigger("requested")
    with pytest.raises(JobCancelled):
        decode.verify_source(path, path.stat().st_size, sha256_of(path), token, noop)
    with pytest.raises(JobCancelled):
        decode.decode_source(path, token, noop)


def test_verify_source_failures(tmp_path: Path) -> None:
    path = write_wav(tmp_path / "a.wav", 44_100, 1.0)
    size, digest = path.stat().st_size, sha256_of(path)

    for args, code in (
        ((path, size + 1, digest), "source_changed"),
        ((path, size, "0" * 64), "source_changed"),
        ((tmp_path / "gone.wav", size, digest), "source_unreadable"),
    ):
        with pytest.raises(JobFailure) as error:
            decode.verify_source(*args, CancelToken(), noop)
        assert error.value.code == code


def test_the_source_is_never_modified(tmp_path: Path) -> None:
    path = write_wav(tmp_path / "a.wav", 44_100, 1.0)
    before = (path.stat().st_mtime_ns, sha256_of(path))

    decode.verify_source(path, path.stat().st_size, before[1], CancelToken(), noop)
    decode.decode_source(path, CancelToken(), noop)

    assert (path.stat().st_mtime_ns, sha256_of(path)) == before
