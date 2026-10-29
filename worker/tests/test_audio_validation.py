"""Input decoding, normalization tolerance and output validation (in-process, no model)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from audio_fixtures import synth_song, write_mp3, write_wav
from snds_worker.audio import (
    MODEL_SR,
    decode_source,
    fit_length,
    parse_mp3_info,
    resample,
    to_stereo,
)
from snds_worker.model import SOURCES
from snds_worker.protocol import ErrorCode, WorkerError
from snds_worker.validation import alignment, band_residual_db, validate_stems

GAINS = (0.4, 0.3, 0.2, 0.1)


# --- normalization tolerance: exact frames, lag 0, negligible in-band error -------------------


@pytest.mark.parametrize("sr", [8000, 22050, 44100, 48000, 96000, 192000])
def test_resample_round_trip_is_exact_length_and_aligned(sr: int) -> None:
    frames = sr * 6 + 777
    x = synth_song(frames / sr + 0.01, sr=sr, seed=sr)[:frames]
    x44 = resample(x, sr, MODEL_SR)
    back = fit_length(resample(x44, MODEL_SR, sr), frames)
    assert back.shape == (frames, 2)
    assert back.dtype == np.float32
    align = alignment(x.mean(axis=1), back.mean(axis=1), sr)
    assert align["lag_samples"] == 0
    assert all(w["lag"] == 0 for w in align["windows"])
    assert abs(align["lag_subsample"]) <= 0.05
    inband, _ = band_residual_db(x, back, sr, min(20000.0, 0.45 * sr))
    assert inband is not None and inband < -40.0


def test_fit_length_trims_and_pads_along_axis() -> None:
    x = np.ones((2, 10), np.float32)
    assert fit_length(x, 7, axis=1).shape == (2, 7)
    padded = fit_length(x, 12, axis=1)
    assert padded.shape == (2, 12) and padded[:, 10:].sum() == 0
    assert fit_length(x, 10, axis=1) is x


def test_to_stereo_duplicates_mono() -> None:
    m = np.arange(5, dtype=np.float32)[:, None]
    s = to_stereo(m)
    assert s.shape == (5, 2) and np.array_equal(s[:, 0], s[:, 1])


# --- input validation ----------------------------------------------------------------------


@pytest.mark.parametrize(
    ("sr", "ch", "subtype"),
    [
        (44100, 2, "PCM_16"),
        (48000, 2, "PCM_24"),
        (96000, 1, "PCM_32"),
        (22050, 1, "FLOAT"),
        (192000, 2, "DOUBLE"),
        (8000, 1, "PCM_U8"),
    ],
)
def test_supported_wav_variants_decode(tmp_path: Path, sr: int, ch: int, subtype: str) -> None:
    frames = sr + 13
    x = synth_song(frames / sr + 0.01, sr=sr, channels=ch)[:frames]
    src = decode_source(write_wav(tmp_path / "s.wav", x, sr, subtype))
    assert (src.samplerate, src.channels, src.frames, src.header_frames) == (sr, ch, frames, frames)
    assert src.subtype == subtype and src.mp3 is None
    assert src.audio.dtype == np.float32 and src.audio.shape == (frames, ch)


def test_rf64_wav_decodes(tmp_path: Path) -> None:
    # The app accepts RF64 (large-file) WAVs, so the worker must too.
    frames = 48000 + 7
    x = synth_song(frames / 48000 + 0.01, sr=48000)[:frames]
    path = tmp_path / "s.wav"
    sf.write(str(path), x, 48000, format="RF64", subtype="PCM_24")
    src = decode_source(path)
    assert src.format == "RF64" and (src.frames, src.channels) == (frames, 2)


def test_mp3_decoded_frames_and_gapless_info(tmp_path: Path) -> None:
    frames = 44100 * 3 + 345
    x = synth_song(frames / 44100 + 0.01, seed=5)[:frames]
    path = write_mp3(tmp_path / "s.mp3", x, 44100)
    src = decode_source(path)
    assert src.format == "MP3" and src.subtype == "MPEG_LAYER_III"
    assert src.frames == frames  # gapless LAME tag honoured by libsndfile/mpg123
    info = parse_mp3_info(path)
    assert info["gapless_info"] is True
    assert info["gapless_frames"] == frames
    assert info["samples_per_frame"] == 1152 and info["info_tag"]["encoder_delay"] == 576


def _expect(code: ErrorCode, path: Path) -> None:
    with pytest.raises(WorkerError) as ei:
        decode_source(path)
    assert ei.value.code == code, ei.value.message


def test_rejects_multichannel(tmp_path: Path) -> None:
    x = np.zeros((4410, 3), np.float32)
    _expect(ErrorCode.UNSUPPORTED_CHANNEL_COUNT, write_wav(tmp_path / "3ch.wav", x, 44100))


def test_rejects_compressed_wav_subtypes(tmp_path: Path) -> None:
    x = synth_song(0.5, sr=8000)
    _expect(ErrorCode.UNSUPPORTED_FORMAT, write_wav(tmp_path / "ulaw.wav", x, 8000, "ULAW"))
    _expect(ErrorCode.UNSUPPORTED_FORMAT, write_wav(tmp_path / "adpcm.wav", x, 8000, "IMA_ADPCM"))


def test_rejects_other_containers_and_garbage(tmp_path: Path) -> None:
    x = synth_song(0.5)
    flac = tmp_path / "s.flac"
    sf.write(str(flac), x, 44100, format="FLAC")
    _expect(ErrorCode.UNSUPPORTED_FORMAT, flac)
    junk = tmp_path / "junk.wav"
    junk.write_bytes(b"this is not audio at all" * 100)
    _expect(ErrorCode.UNSUPPORTED_FORMAT, junk)


def test_rejects_sample_rates_out_of_range(tmp_path: Path) -> None:
    _expect(
        ErrorCode.UNSUPPORTED_SAMPLE_RATE, write_wav(tmp_path / "4k.wav", np.zeros((400, 2)), 4000)
    )
    _expect(
        ErrorCode.UNSUPPORTED_SAMPLE_RATE,
        write_wav(tmp_path / "384k.wav", np.zeros((400, 2)), 384000),
    )


def test_rejects_empty_and_non_finite(tmp_path: Path) -> None:
    _expect(ErrorCode.SOURCE_EMPTY, write_wav(tmp_path / "empty.wav", np.zeros((0, 2)), 44100))
    x = np.zeros((1000, 2), np.float32)
    x[500, 0] = np.nan
    _expect(ErrorCode.DECODE_FAILED, write_wav(tmp_path / "nan.wav", x, 44100, "FLOAT"))


# --- output validation ---------------------------------------------------------------------


def _package(
    tmp_path: Path, sr: int = 48000, seconds: float = 4.0, **mutations
) -> tuple[Path, np.ndarray]:
    frames = int(sr * seconds) + 11
    src = synth_song(frames / sr + 0.01, sr=sr, seed=9)[:frames]
    stems_dir = tmp_path / "stems"
    stems_dir.mkdir(parents=True, exist_ok=True)
    for name, g in zip(SOURCES, GAINS, strict=True):
        y = (src * np.float32(g)).astype(np.float32)
        y = mutations.get(name, lambda a: a)(y)
        sf.write(
            str(stems_dir / f"{name}.wav"),
            y,
            mutations.get("rate", sr),
            format="WAV",
            subtype=mutations.get("subtype", "FLOAT"),
        )
    return stems_dir, src


def _codes(report) -> list[str]:
    return [f["code"] for f in report.failures]


def test_valid_package_passes(tmp_path: Path) -> None:
    stems_dir, src = _package(tmp_path)
    report = validate_stems(stems_dir, source=src, samplerate=48000)
    assert report.ok and report.status == "passed", report.failures
    assert report.metrics["alignment"]["lag_samples"] == 0
    assert report.metrics["residual_db_inband"] < -60
    for name in SOURCES:
        s = report.stems[name]
        assert s["bytes"] == (stems_dir / f"{name}.wav").stat().st_size
        assert len(s["sha256"]) == 64 and s["frames"] == src.shape[0]


def test_wrong_frame_count_fails(tmp_path: Path) -> None:
    stems_dir, src = _package(tmp_path, vocals=lambda y: y[:-1])
    assert "wrong-frame-count" in _codes(validate_stems(stems_dir, source=src, samplerate=48000))


def test_nan_fails(tmp_path: Path) -> None:
    def poison(y: np.ndarray) -> np.ndarray:
        y = y.copy()
        y[100, 1] = np.nan
        return y

    stems_dir, src = _package(tmp_path, bass=poison)
    assert "non-finite-samples" in _codes(validate_stems(stems_dir, source=src, samplerate=48000))


def test_missing_stem_fails(tmp_path: Path) -> None:
    stems_dir, src = _package(tmp_path)
    (stems_dir / "other.wav").unlink()
    report = validate_stems(stems_dir, source=src, samplerate=48000)
    assert _codes(report) == ["missing-stem"]


def test_wrong_rate_channels_and_subtype_fail(tmp_path: Path) -> None:
    stems_dir, src = _package(tmp_path / "a", rate=44100)
    assert "wrong-sample-rate" in _codes(validate_stems(stems_dir, source=src, samplerate=48000))
    stems_dir, src = _package(tmp_path / "b", subtype="PCM_16")
    assert "wrong-format" in _codes(validate_stems(stems_dir, source=src, samplerate=48000))
    stems_dir, src = _package(tmp_path / "c", drums=lambda y: y[:, :1])
    assert "wrong-channel-count" in _codes(validate_stems(stems_dir, source=src, samplerate=48000))


def test_offset_stems_are_misaligned(tmp_path: Path) -> None:
    shift = lambda y: np.concatenate([np.zeros((5, 2), np.float32), y[:-5]])  # noqa: E731
    stems_dir, src = _package(tmp_path, **{name: shift for name in SOURCES})
    report = validate_stems(stems_dir, source=src, samplerate=48000)
    assert "misaligned" in _codes(report)
    assert report.metrics["alignment"]["lag_samples"] == 5


def test_residual_thresholds(tmp_path: Path) -> None:
    # vocals carries 10 % of the mix; scaling it changes the stem-sum error.
    stems_dir, src = _package(
        tmp_path, vocals=lambda y: y * np.float32(2.5)
    )  # error 15 % -> -16.5 dB
    report = validate_stems(stems_dir, source=src, samplerate=48000)
    assert report.ok and report.status == "passed-with-warnings"
    assert [w["code"] for w in report.warnings] == ["residual-high"]
    stems_dir, src = _package(tmp_path / "b", vocals=lambda y: y * np.float32(6.0))  # 50 % -> -6 dB
    assert "residual-too-high" in _codes(validate_stems(stems_dir, source=src, samplerate=48000))


def test_silent_source_skips_alignment(tmp_path: Path) -> None:
    stems_dir = tmp_path / "stems"
    stems_dir.mkdir()
    silent = np.zeros((48000 * 2, 2), np.float32)
    for name in SOURCES:
        sf.write(str(stems_dir / f"{name}.wav"), silent, 48000, subtype="FLOAT")
    report = validate_stems(stems_dir, source=silent, samplerate=48000)
    assert report.ok
    assert report.metrics["alignment"]["applicable"] is False
    assert report.metrics["residual_db_inband"] is None
