from __future__ import annotations

import errno
import os
from pathlib import Path

import pytest

from helpers import synth_audio
from snds_worker import model, normalize, separation
from snds_worker.cancel import CancelToken
from snds_worker.decode import DecodedSource
from snds_worker.errors import JobCancelled, JobFailure

# --- Device choice -------------------------------------------------------------------------


def test_cpu_preference_never_touches_cuda(monkeypatch: pytest.MonkeyPatch) -> None:
    import torch

    monkeypatch.setattr(torch.cuda, "is_available", lambda: pytest.fail("must not be asked"))

    assert separation.resolve_device("cpu") == "cpu"


def test_auto_uses_cuda_only_when_a_kernel_runs(monkeypatch: pytest.MonkeyPatch) -> None:
    import torch

    monkeypatch.setattr(torch.version, "cuda", "12.1")
    monkeypatch.setattr(torch.cuda, "is_available", lambda: True)
    real_randn = torch.randn
    calls = []

    def fake_randn(*args, device=None, **kwargs):
        calls.append(device)
        return real_randn(*args, **kwargs)  # computed on the CPU, "as if" CUDA worked

    monkeypatch.setattr(torch, "randn", fake_randn)
    monkeypatch.setattr(torch.cuda, "synchronize", lambda *_a: None)

    assert separation.resolve_device("auto") == "cuda"
    assert calls == ["cuda"]


def test_auto_falls_back_when_the_kernel_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    import torch

    monkeypatch.setattr(torch.version, "cuda", "12.1")
    monkeypatch.setattr(torch.cuda, "is_available", lambda: True)

    def broken(*_args, **_kwargs):
        raise RuntimeError("CUDA error: unknown error")

    monkeypatch.setattr(torch, "randn", broken)

    assert separation.resolve_device("auto") == "cpu"


def test_auto_is_cpu_without_cuda(monkeypatch: pytest.MonkeyPatch) -> None:
    import torch

    monkeypatch.setattr(torch.cuda, "is_available", lambda: False)

    assert separation.resolve_device("auto") == "cpu"


# --- Error classification ------------------------------------------------------------------


@pytest.mark.parametrize(
    ("error", "device", "code", "retryable", "action"),
    [
        (
            RuntimeError("CUDA out of memory. Tried to allocate 1 GiB"),
            "cuda",
            "out_of_memory",
            True,
            "retry_cpu",
        ),
        (MemoryError(), "cuda", "out_of_memory", True, "retry_cpu"),
        (MemoryError(), "cpu", "out_of_memory", False, None),
        (
            RuntimeError("[enforce fail at alloc_cpu.cpp] DefaultCPUAllocator: not enough memory"),
            "cpu",
            "out_of_memory",
            False,
            None,
        ),
        (
            RuntimeError("CUDA error: device-side assert triggered"),
            "cuda",
            "cuda_failed",
            True,
            "retry_cpu",
        ),
        (
            RuntimeError("cuDNN error: CUDNN_STATUS_EXECUTION_FAILED"),
            "cuda",
            "cuda_failed",
            True,
            "retry_cpu",
        ),
        (
            RuntimeError("CUDA mentioned but running on cpu"),
            "cpu",
            "separation_failed",
            False,
            None,
        ),
        (OSError(errno.ENOSPC, "No space left"), "cpu", "disk_full", True, "free_disk_space"),
        (ValueError("bad shape"), "cuda", "separation_failed", False, None),
    ],
)
def test_classify_error(error, device, code, retryable, action) -> None:
    failure = separation.classify_error(error, device)

    assert failure.code == code
    assert failure.retryable is retryable
    assert failure.suggested_action == action
    assert failure.message


def test_torch_cuda_out_of_memory_type_is_recognized() -> None:
    class OutOfMemoryError(RuntimeError):  # torch.cuda.OutOfMemoryError by name
        pass

    assert separation.classify_error(OutOfMemoryError("x"), "cuda").code == "out_of_memory"


# --- Segment pool and the real checkpoint wiring ---------------------------------------------


def test_segment_pool_reports_progress_and_polls_cancellation() -> None:
    seen: list[float] = []
    token = CancelToken()
    pool = separation._SegmentPool(4, seen.append, token)

    futures = [
        pool.submit(
            lambda value=value: value * 2,
        )
        for value in range(4)
    ]
    assert [f.result() for f in futures[:2]] == [0, 2]
    assert seen == [0.25, 0.5]

    token.trigger("requested")
    with pytest.raises(JobCancelled):
        futures[2].result()
    assert seen == [0.25, 0.5]  # the cancelled segment never ran


def test_demucs_separator_reports_a_missing_checkpoint(tmp_path: Path) -> None:
    with pytest.raises(JobFailure) as error:
        separation.DemucsSeparator("cpu", tmp_path)

    assert error.value.code == "model_missing"
    assert error.value.suggested_action == "fetch_model"


def test_demucs_separator_rejects_a_corrupt_checkpoint(tmp_path: Path) -> None:
    (tmp_path / model.MODEL_FILENAME).write_bytes(b"truncated")

    with pytest.raises(JobFailure) as error:
        separation.DemucsSeparator("cpu", tmp_path)

    assert error.value.code == "model_missing"


# --- Normalization ---------------------------------------------------------------------------


@pytest.mark.parametrize("rate", [11_025, 22_050, 44_100, 48_000, 88_200, 96_000, 192_000])
@pytest.mark.parametrize("channels", [1, 2])
def test_normalization_gives_exact_frame_counts(tmp_path: Path, rate: int, channels: int) -> None:
    import soundfile as sf
    import torch

    audio = synth_audio(rate, 2.37, channels)
    decoded = DecodedSource(audio, rate, channels, len(audio), "wav", "FLOAT")

    model_input = normalize.to_model_input(decoded)
    assert model_input.shape[0] == 2
    assert model_input.dtype == torch.float32

    written = normalize.write_stem("drums", model_input * 0.5, decoded, tmp_path)

    info = sf.info(str(written.path))
    assert (info.frames, info.samplerate, info.channels, info.subtype) == (
        len(audio),
        rate,
        2,
        "FLOAT",
    )
    assert written.padded_frames == 0 and written.trimmed_frames == 0


def test_normalization_rejects_non_finite_samples(tmp_path: Path) -> None:
    import torch

    audio = synth_audio(44_100, 1.0)
    decoded = DecodedSource(audio, 44_100, 2, len(audio), "wav", "FLOAT")
    stem = torch.zeros(2, len(audio))
    stem[0, 3] = float("inf")

    with pytest.raises(JobFailure) as error:
        normalize.write_stem("bass", stem, decoded, tmp_path)

    assert error.value.code == "normalization_failed"


def test_normalization_rejects_large_misalignment(tmp_path: Path) -> None:
    import torch

    audio = synth_audio(44_100, 1.0)
    decoded = DecodedSource(audio, 44_100, 2, len(audio), "wav", "FLOAT")

    with pytest.raises(JobFailure, match="line up"):
        normalize.write_stem("bass", torch.zeros(2, len(audio) - 50), decoded, tmp_path)


# --- Real checkpoint (opt in) ----------------------------------------------------------------

MODEL_DIR = Path(
    os.environ.get(
        "SNDS_TEST_MODEL_DIR",
        str(Path(os.environ.get("LOCALAPPDATA", ".")) / "snds-spikes" / "htdemucs-artifact"),
    )
)


@pytest.mark.skipif(
    os.environ.get("SNDS_RUN_MODEL_TESTS") != "1",
    reason="set SNDS_RUN_MODEL_TESTS=1 to run the real htdemucs checkpoint",
)
def test_real_htdemucs_separates_and_stays_aligned(tmp_path: Path) -> None:
    from helpers import run_job, write_wav

    source = write_wav(tmp_path / "song.wav", 48_000, 9.0)
    seen = []

    run = run_job(
        tmp_path,
        source,
        factory=lambda device, _dir: separation.DemucsSeparator(device, MODEL_DIR),
        on_event=lambda e: seen.append(e) if e["type"] == "progress" else None,
    )

    assert run.exit_code == 0, run.terminal
    assert run.job.validation.alignment["startLag"] == 0
    assert any(e["stage"] == "separating" and e["fraction"] == 1.0 for e in seen)
