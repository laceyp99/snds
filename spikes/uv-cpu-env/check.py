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
    assert torch.__version__ == "2.1.2+cpu"
    assert torchaudio.__version__ == "2.1.2+cpu"
    assert not torch.cuda.is_available()

    values = numpy.array([1.0, 2.0], dtype=numpy.float32)
    assert torch.from_numpy(values).tolist() == [1.0, 2.0]
    assert "soundfile" in torchaudio.list_audio_backends()

    wav_path = pathlib.Path(tempfile.gettempdir()) / "snds-uv-cpu-spike.wav"
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
    print("cpu_environment=ready")


if __name__ == "__main__":
    main()
