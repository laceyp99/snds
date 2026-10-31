# `htdemucs` model artifact spike

This spike resolves the Demucs 4.0.1 public model name `htdemucs` to its exact
checkpoint, downloads it outside the repository, verifies its checksum, and
loads it through Demucs' local repository path.

The model registry included in Demucs 4.0.1 defines `htdemucs` as a one-model
bag containing signature `955717e8`. The upstream checkpoint filename is
`955717e8-8726e21a.th`; Demucs treats the suffix as a SHA-256 prefix.

The complete verified SHA-256 is:

```text
8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4
```

The checkpoint is 84,141,911 bytes.

## Reproduce

CPU:

```powershell
$python = "$env:LOCALAPPDATA\snds-spikes\uv-cpu-clean-20260718-b\Scripts\python.exe"
& $python .\spikes\htdemucs-artifact\check.py --device cpu
```

CUDA:

```powershell
$python = "$env:LOCALAPPDATA\snds-spikes\uv-cuda-clean-20260718\Scripts\python.exe"
& $python .\spikes\htdemucs-artifact\check.py --device cuda
```

By default the checkpoint is stored under
`%LOCALAPPDATA%\snds-spikes\htdemucs-artifact`, not in the source tree.

## Synthetic inference smoke test

```powershell
& $python .\spikes\htdemucs-artifact\benchmark.py --device cpu
& $python .\spikes\htdemucs-artifact\benchmark.py --device cuda
```

On 2026-07-19, a deterministic two-second stereo input produced four finite,
stereo, sample-aligned outputs in both environments. One cold run measured
5.583 seconds on CPU and 5.950 seconds on CUDA. This tiny input is dominated by
startup and transfer overhead and is not a meaningful throughput comparison.

On 2026-10-03, a 60-second synthetic input (`--duration 60`) measured 29.008
seconds on CPU (Intel Core i5-13500H, 12 cores, 32 GB) and 3.215 seconds on
CUDA (RTX 4050 Laptop GPU, driver 560.94), excluding model load. Demucs runtime
depends on input length rather than content, so these ratios (about 0.5x
real time on CPU and 0.05x on CUDA) are a usable first estimate. A six-core
baseline is expected to be slower than this 12-core laptop CPU.

A representative music benchmark remains pending a short rights-cleared test
track. The synthetic test proves execution, shape, and throughput contracts,
not separation quality.
