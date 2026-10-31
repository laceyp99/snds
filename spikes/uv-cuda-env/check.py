from __future__ import annotations

import pathlib
import sys
import tempfile

import demucs
import numpy
import soundfile
import torch
import torchaudio


def main() -> None:
    assert torch.__version__ == "2.1.2+cu121"
    assert torchaudio.__version__ == "2.1.2+cu121"
    assert torch.version.cuda == "12.1"
    assert torch.cuda.is_available()
    assert torch.cuda.device_count() >= 1

    device = torch.device("cuda:0")
    left = torch.tensor([1.0, 2.0, 3.0], device=device)
    right = torch.tensor([4.0, 5.0, 6.0], device=device)
    product = torch.dot(left, right)
    torch.cuda.synchronize(device)
    assert product.cpu().item() == 32.0

    try:
        torch.zeros(1, device="cuda:999")
    except RuntimeError:
        retry = torch.tensor([2.0, 3.0], device="cpu")
        assert torch.prod(retry).item() == 6.0
    else:
        raise AssertionError("The intentional invalid CUDA device did not fail")

    cpu_values = numpy.array([1.0, 2.0], dtype=numpy.float32)
    assert torch.from_numpy(cpu_values).tolist() == [1.0, 2.0]
    assert "soundfile" in torchaudio.list_audio_backends()

    wav_path = pathlib.Path(tempfile.gettempdir()) / "snds-uv-cuda-spike.wav"
    try:
        audio = torch.zeros((2, 4_410))
        torchaudio.save(str(wav_path), audio, 44_100, backend="soundfile")
        loaded, sample_rate = torchaudio.load(
            str(wav_path), backend="soundfile"
        )
        assert loaded.shape == audio.shape
        assert sample_rate == 44_100
    finally:
        wav_path.unlink(missing_ok=True)

    print(f"python={sys.version.split()[0]}")
    print(f"numpy={numpy.__version__}")
    print(f"torch={torch.__version__}")
    print(f"torchaudio={torchaudio.__version__}")
    print(f"demucs={demucs.__version__}")
    print(f"soundfile={soundfile.__version__}")
    print(f"cuda_runtime={torch.version.cuda}")
    print(f"cuda_device={torch.cuda.get_device_name(device)}")
    print("cuda_environment=ready")
    print("intentional_cuda_failure=handled")
    print("explicit_cpu_retry=ready")


if __name__ == "__main__":
    main()
