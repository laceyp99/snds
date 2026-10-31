from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from helpers import run_job, write_wav
from snds_worker import decode, validate
from snds_worker.cancel import CancelToken
from snds_worker.errors import JobCancelled, JobFailure

STEMS = ("drums", "bass", "other", "vocals")


@pytest.fixture
def package(tmp_path: Path):
    """A valid package from the fake separator, and its decoded source."""
    source_path = write_wav(tmp_path / "song.wav", 44_100, 10.0)
    run = run_job(tmp_path, source_path)
    assert run.exit_code == 0, run.events[-1]
    source = decode.decode_source(source_path, CancelToken(), lambda _f: None)
    return run.package, source


def check(package, source, token=None):
    return validate.validate_stems(package, source, token or CancelToken(), lambda _f: None)


def rewrite(path: Path, transform, **write_args) -> None:
    audio, rate = sf.read(str(path), dtype="float32", always_2d=True)
    sf.write(str(path), transform(audio), write_args.pop("rate", rate), **write_args)


def test_valid_package_reports_metrics(package) -> None:
    directory, source = package

    report = check(directory, source)

    assert list(report.stems) == list(STEMS)
    for name, stem in report.stems.items():
        assert stem.file == f"{name}.wav"
        assert (stem.frames, stem.sample_rate, stem.channels) == (source.frames, 44_100, 2)
        assert stem.peak > 0
    assert report.alignment["startLag"] == 0
    assert report.alignment["endLag"] == 0
    assert abs(report.alignment["driftSamples"]) < 0.05
    assert report.alignment["startCoefficient"] > 0.99
    assert report.warnings == []


def test_missing_stem(package) -> None:
    directory, source = package
    (directory / "vocals.wav").unlink()

    with pytest.raises(JobFailure) as error:
        check(directory, source)

    assert error.value.code == "validation_failed"
    assert "exactly" in error.value.message


def test_extra_audio_file(package) -> None:
    directory, source = package
    (directory / "extra.wav").write_bytes((directory / "bass.wav").read_bytes())

    with pytest.raises(JobFailure, match="exactly"):
        check(directory, source)


def test_unreadable_stem(package) -> None:
    directory, source = package
    (directory / "bass.wav").write_bytes(b"not a wav file")

    with pytest.raises(JobFailure, match="bass stem cannot be read"):
        check(directory, source)


@pytest.mark.parametrize(
    ("what", "transform", "args"),
    [
        ("channels", lambda a: a[:, :1], {}),
        ("sample format", lambda a: a, {"subtype": "PCM_16"}),
        ("sample rate", lambda a: a, {"rate": 48_000}),
        ("frames", lambda a: a[:-1], {}),
    ],
)
def test_stem_format_violations(package, what, transform, args) -> None:
    directory, source = package
    args = {"subtype": "FLOAT", **args}
    rewrite(directory / "other.wav", transform, **args)

    with pytest.raises(JobFailure) as error:
        check(directory, source)

    assert error.value.code == "validation_failed"
    assert what in error.value.details


def test_non_finite_stem(package) -> None:
    directory, source = package

    def poison(audio):
        audio[1234, 1] = np.inf
        return audio

    rewrite(directory / "drums.wav", poison, subtype="FLOAT")

    with pytest.raises(JobFailure, match="invalid"):
        check(directory, source)


def test_stem_that_does_not_belong_to_the_source(package) -> None:
    directory, source = package
    rng = np.random.default_rng(1)
    rewrite(
        directory / "drums.wav",
        lambda a: rng.standard_normal(a.shape).astype(np.float32),
        subtype="FLOAT",
    )

    with pytest.raises(JobFailure, match="do not match"):
        check(directory, source)


# The gate is on the sum of stems, so it catches a separator that shifts the stems as a whole
# (what a resampling or padding bug does). Per-stem lag is a spike diagnostic, not a gate.
@pytest.mark.parametrize("shift", [-400, -1, 1, 25])
def test_stems_shifted_against_the_source(package, shift) -> None:
    directory, source = package
    for name in STEMS:
        rewrite(directory / f"{name}.wav", lambda a: np.roll(a, shift, axis=0), subtype="FLOAT")

    with pytest.raises(JobFailure) as error:
        check(directory, source)

    assert error.value.code == "validation_failed"
    assert "not aligned" in error.value.message


def test_peaks_above_one_are_valid(package) -> None:
    directory, source = package
    # Scale one stem past full scale and compensate another so the sum is unchanged.
    drums, rate = sf.read(str(directory / "drums.wav"), dtype="float32")
    bass, _ = sf.read(str(directory / "bass.wav"), dtype="float32")
    sf.write(str(directory / "drums.wav"), drums + 3.0, rate, subtype="FLOAT")
    sf.write(str(directory / "bass.wav"), bass - 3.0, rate, subtype="FLOAT")

    report = check(directory, source)

    assert report.stems["drums"].peak > 1.0


def test_cancellation_is_polled_during_validation(package) -> None:
    directory, source = package
    token = CancelToken()
    token.trigger("requested")

    with pytest.raises(JobCancelled):
        check(directory, source, token)


def test_xcorr_lag_recovers_integer_shifts() -> None:
    rng = np.random.default_rng(3)
    reference = np.convolve(rng.standard_normal(5000), np.ones(8) / 8, mode="same")

    for shift in (-7, 0, 5):
        lag, sub, coefficient = validate.xcorr_lag(reference, np.roll(reference, shift), 20)
        assert lag == shift
        assert abs(sub - shift) < 0.5
        assert coefficient > 0.95
