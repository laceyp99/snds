# CPU environment spike

This disposable spike tests whether the proposed SNDS Python worker stack can be
resolved and run on Windows without changing global Python packages. It is not
the Phase 1 worker project.

## Candidate pins

- CPython 3.11
- PyTorch 2.1.2 CPU
- TorchAudio 2.1.2 CPU
- Demucs 4.0.1
- NumPy 1.26.4
- Packaging 25.0
- SoundFile 0.12.1

NumPy is pinned below 2 because the selected PyTorch wheel was built against
NumPy 1.x. SoundFile is explicit because the Windows TorchAudio CPU wheel did
not expose a working WAV backend without it. Packaging is explicit because
Torch 2.1.2 performs version comparisons through it but does not declare it as
a required dependency.

## Reproduce

Keep the environment outside the repository and sync only from the lockfile:

```powershell
$env:UV_PROJECT_ENVIRONMENT = "$env:LOCALAPPDATA\snds-spikes\uv-cpu-clean"
uv sync --project .\spikes\uv-cpu-env --locked --python 3.11
uv run --project .\spikes\uv-cpu-env --locked python .\spikes\uv-cpu-env\check.py
Remove-Item Env:UV_PROJECT_ENVIRONMENT
```

The check covers package imports, CPU-only Torch selection, the NumPy bridge,
and stereo WAV save/load through the SoundFile backend. Model acquisition and
an actual separation benchmark belong to the separate checkpoint spike.

## Result

Passed on Windows 11 x64 on 2026-07-18 by syncing a new environment from
`uv.lock`. The test did not download `htdemucs`, process source audio, measure
CPU runtime, or validate MP3 decoding; those remain separate Phase 0 risks.

## Clean bootstrap verification

Repeated on 2026-10-03 with no pre-existing project environment:

- `uv sync --locked --python 3.11 --no-cache` into a new
  `UV_PROJECT_ENVIRONMENT` downloaded every wheel, built the three sdists
  (`demucs`, `dora-search`, `antlr4-python3-runtime`), and finished in 55
  seconds. `check.py` passed.
- `uv sync --locked --python 3.11 --managed-python` into another new
  environment used a uv-managed CPython 3.11.15 instead of the system 3.11.9
  interpreter. `check.py` passed.

The lock is therefore independent of the uv cache and of a system Python. Patch
releases of CPython 3.11 differ between those runs, so the worker project
should pin an exact interpreter with `.python-version` and `--managed-python`.
