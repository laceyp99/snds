# CUDA environment spike

This disposable spike tests whether the proposed SNDS Python worker stack can
use an NVIDIA GPU on Windows without changing global Python or CUDA packages.
It is deliberately separate from the CPU environment and is not the Phase 1
worker project.

## Candidate pins

- CPython 3.11
- PyTorch 2.1.2 with CUDA 12.1 runtime
- TorchAudio 2.1.2 with CUDA 12.1 runtime
- Demucs 4.0.1
- NumPy 1.26.4
- Packaging 25.0
- SoundFile 0.12.1

The PyTorch wheel supplies its CUDA runtime. The locally installed CUDA Toolkit
is not mutated and is not used to resolve this environment. A sufficiently new
NVIDIA display driver is still required. CUDA 12.1 minor-version compatibility
requires NVIDIA Windows driver 527.41 or newer.

## Reproduce

Keep the environment outside the repository and sync only from the lockfile:

```powershell
$env:UV_PROJECT_ENVIRONMENT = "$env:LOCALAPPDATA\snds-spikes\uv-cuda-clean"
uv sync --project .\spikes\uv-cuda-env --locked --python 3.11
uv run --project .\spikes\uv-cuda-env --locked python .\spikes\uv-cuda-env\check.py
Remove-Item Env:UV_PROJECT_ENVIRONMENT
```

The check verifies CUDA discovery, a synchronized GPU tensor operation, an
intentional invalid-device failure followed by an explicit CPU retry, and
stereo WAV I/O. It does not download or run the `htdemucs` model; that belongs
to the checkpoint spike.

## Result

Passed on Windows 11 x64 on 2026-07-18 with an NVIDIA GeForce RTX 4050 Laptop
GPU and NVIDIA driver 560.94. PyTorch reported its bundled CUDA 12.1 runtime;
the machine's separately installed CUDA Toolkit was not required by the check.

## CUDA fallback verification

Repeated on 2026-10-03 on the same hardware:

- `check.py` passed again: CUDA was detected, the intentional `cuda:999`
  allocation failed with `RuntimeError`, and the explicit CPU retry succeeded.
- With `CUDA_VISIBLE_DEVICES=-1`, the CUDA build of PyTorch reported
  `torch.cuda.is_available() == False` and CPU tensor work succeeded. A CUDA
  environment can therefore serve both the force-CPU setting and the CPU
  fallback without a second environment.
