"""The owner-supplied music excerpt through the real worker process, with the fake separator.

testdata/asking42much-excerpt.wav and .mp3 are 10 s of a real mix (see testdata/README.md). The fake
separator splits the mixture into four stems that sum back to it exactly, so everything around the
separation (decoding, the MP3 gapless trim, publishing, the package on disk) is checked on real
music without the model. The real-model run on the same excerpt is opt-in: see docs/testing.md.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from procs import check_stream, request_for_source
from snds_worker import finalize, jobs_protocol, schema

TESTDATA = Path(__file__).resolve().parents[2] / "testdata"
STEMS = ("drums", "bass", "other", "vocals")
EXCERPT_FRAMES = 441_000  # 10.000 s at 44.1 kHz

# Pinned to the committed bytes; also recorded in testdata/README.md.
SHA256 = {
    "wav": "5b859d9c2a3e321b5470aa5959283ec3c8c15e2426c5f650d0a8d4d422d267aa",
    "mp3": "d8eeb337575c78ed99c3bfbf20c09aeb64d0e2a2d88105663465dc0b12902752",
}


def excerpt(kind: str) -> Path:
    path = TESTDATA / f"asking42much-excerpt.{kind}"
    assert path.is_file(), f"missing fixture {path}"
    return path


@pytest.mark.parametrize("kind", ["wav", "mp3"])
def test_the_committed_excerpt_is_the_one_recorded(kind: str) -> None:
    path = excerpt(kind)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == SHA256[kind]
    info = sf.info(str(path))
    # libsndfile applies the MP3's LAME gapless tag, so both files are exactly ten seconds.
    assert (info.frames, info.samplerate, info.channels) == (EXCERPT_FRAMES, 44_100, 2)
    if kind == "wav":
        assert info.subtype == "PCM_16"
        assert path.stat().st_size == 44 + EXCERPT_FRAMES * 4


@pytest.mark.parametrize("kind", ["wav", "mp3"])
def test_prepare_publishes_the_excerpt(tmp_path: Path, started, kind: str) -> None:
    source = excerpt(kind)
    request = request_for_source(source, tmp_path)
    job = started()

    job.send(request)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert code == jobs_protocol.EXIT_OK, events[-1]
    assert events[-1]["type"] == "result"
    cache = Path(request["cacheDir"])
    assert finalize.verify_package(cache, cache.name)[0] == schema.COMPLETE
    assert (
        cache.name == f"{SHA256[kind]}-{source.stat().st_size}"
    )  # the content hash names the cache entry

    data = json.loads((cache / "song.json").read_text(encoding="utf-8"))
    assert data["source"]["container"] == kind
    assert data["source"]["sha256"] == SHA256[kind]
    assert data["source"]["sizeBytes"] == source.stat().st_size
    assert (data["source"]["frames"], data["source"]["sampleRate"], data["source"]["channels"]) == (
        EXCERPT_FRAMES,
        44_100,
        2,
    )

    # Four stems of the source's length and rate, float32, that sum back to the decoded source.
    mixture, rate = sf.read(str(source), dtype="float32", always_2d=True)
    assert rate == 44_100 and len(mixture) == EXCERPT_FRAMES
    total = np.zeros_like(mixture, dtype=np.float64)
    for name in STEMS:
        info = sf.info(str(cache / f"{name}.wav"))
        assert (info.frames, info.samplerate, info.channels, info.subtype) == (
            EXCERPT_FRAMES,
            44_100,
            2,
            "FLOAT",
        )
        stem, _ = sf.read(str(cache / f"{name}.wav"), dtype="float32", always_2d=True)
        total += stem
    assert np.max(np.abs(total - mixture)) < 1e-5

    # The excerpt is loud, busy music: the stems carry it (nothing is silent, nothing clips).
    assert np.max(np.abs(mixture)) > 0.5
    assert np.sqrt(np.mean(mixture**2)) > 0.05


def test_wav_stems_are_the_pcm_scaled_exactly(tmp_path: Path, started) -> None:
    """The WAV is PCM16: the stems are the samples times a fixed weight, with no drift."""
    source = excerpt("wav")
    request = request_for_source(source, tmp_path)
    job = started()
    job.send(request)
    events, code = job.finish()
    assert code == jobs_protocol.EXIT_OK, events[-1]

    cache = Path(request["cacheDir"])
    pcm, _ = sf.read(str(source), dtype="float32", always_2d=True)
    drums, _ = sf.read(str(cache / "drums.wav"), dtype="float32", always_2d=True)
    scale = np.max(np.abs(drums)) / np.max(np.abs(pcm))
    assert scale == pytest.approx(0.4, rel=1e-3)  # the fake separator's drums weight
    assert np.max(np.abs(drums - 0.4 * pcm)) < 1e-6
